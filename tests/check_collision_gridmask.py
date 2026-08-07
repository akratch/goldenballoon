#!/usr/bin/env python3
"""The collision grid mask must actually filter in Z, or racers fall through the level.

The bug this locks down
-----------------------
`generate_collision_candidates()` (game/src/hasm/collision.c) builds the list of
terrain triangles that `resolve_collisions()` will test.  It pre-filters with a
coarse 8x8 occupancy mask per terrain segment, produced by
`compute_grid_overlap_mask()`: the low byte of the mask is the X columns the query
rectangle spans, the high byte is the Z rows.  A triangle is kept only if it
shares at least one column AND at least one row with the query.

The upstream NON_MATCHING C body's Z loop tested

    cell_z + cell_height >= z1 && z2 >= bbox_z1

and the clamping above it has already forced `z2 >= bbox_z1`, so the second half
is a **tautology**: every Z row from the first accepted one through row 7 gets
set and the Z half of the mask stops filtering anything.  The ROM compares the
rolling per-row value instead, exactly mirroring the X loop
(`compute_grid_overlap_mask`, 0x800315D0: `slt $at, $t5, $t0` with $t5 = the
clamped z2 and $t0 = cell_z).  The matching N64 build uses the assembly, so
upstream never executes the C; this port compiles it (NON_MATCHING=1), so we do.

Why it is silent almost everywhere, and catastrophic in one place
----------------------------------------------------------------
An over-permissive pre-filter still produces CORRECT collisions -- it just feeds
`resolve_collisions()` far too many candidates.  `gCollisionCandidates` holds
`MAX_COLLISION_CANDIDATES` == 500 entries and the fill loop simply **truncates**
(`goto out`) when it is full.  So the defect only bites where a racer's search
rectangle sees more than 500 collidable triangles -- and then the discarded tail
can contain the very facets the racer is standing on.  The ground silently ceases
to exist, all four wheels report SURFACE_NONE, and the racer falls out of the
level.

Measured across all 20 main tracks and all 10 boss tracks (6500 frames each,
MDKR_AUTOPILOT), only **Tricky's arena** saturates the list: boss levels 38 and
46 (the same Fire Mountain spiral, boss 1 and boss 2), where the spiral stacks
many track segments inside one X/Z footprint.  On the other 28 tracks the two
arms' [PACE] racer streams are **byte-identical** -- i.e. the fix changes nothing
it should not.  That neutrality is asserted here too, as the negative control.

Reported symptom, and why the first description was misleading
--------------------------------------------------------------
Reported as *"riding over an elevated edge tilts the vehicle, it never tilts
back, and in places it can then clip through the map"*, then corrected to *"both
me AND the boss clip through the map and fall off at that point, going up the
mountain"*.  Both readings are this one defect.  The pitch/roll a player sees is
derived from where the four wheels touched down, so once the ground is gone there
is nothing to restore them from: measured on level 38 with the broken arm, roll
freezes at exactly 75 for the whole 103-frame fall while pitch ramps to 4793.
With the fix, roll keeps tracking the terrain (75 -> 41 -> 400 -> 95 -> 115) and
pitch peaks at 1797.

Why the check works this way
----------------------------
Both arms are rendered from the SAME binary: `MDKR_GRIDMASK=off` restores the
tautology verbatim (platform/stubs_dkr.c).  The broken arm is therefore
re-produced on every run and has to actually *exhibit* the defect before the
fixed arm is credited with avoiding it, so the check cannot pass vacuously.  It
asserts the mechanism (`[COLL] maxCandidates=/truncated=`), the proximate cause
(`[GRND] gw=0` sustained), and the gameplay outcome (`fin=` -- the boss race is
only finishable at all with the fix).

Three route assumptions were removed ("closedloop" and "objcoll" waves)
-----------------------------------------------------------------------
The DRIVING here was always closed-loop (`MDKR_AUTOPILOT=1`), but assertions
were still pinned to one particular racing line:

1. The original **"at least two racers lost the ground for >= 60 frames"** came
   from one boss-38 line (boss 301 frames, human 103). It first failed when the
   ROM-faithful math shortened the boss's fall to 51 frames. Restoring
   object-model collision then moved that line 2.5 units and prevented its fixed
   arm from finishing at all. The fixture now uses boss 46, the same Fire
   Mountain geometry, and defines a fall purely as a >=2x paired increase over
   the SAME racer's fixed-arm run in the SAME run pair -- no absolute frame
   ceiling on the fixed arm, because Fire Mountain's spiral has legitimate
   airborne stretches and a constant there is a trajectory constant in disguise
   (the same reason the broken arm's peak-y bound was removed). It requires at least one racer:
   the defect is already proven by the 500-entry saturation, truncation, paired
   ground loss, and broken-only failure to finish; requiring both racers to take
   the same bad line measured trajectory, not the grid-mask defect.

       arm, boss 46       boss (pi=-1 ri=1)    human (pi=0 ri=0)
       fixed              18                   27
       broken             51                    2

2. **the win arm used `MDKR_BOSS_SLOW=1`** -- `velocity *= 0.15f` on the boss --
   to make the human finish first.  That is not a closed loop either: the human is
   driven by DKR's own AI, which paths relative to the field, so crippling the
   boss moved the HUMAN's line too.  With the ROM-faithful math it moved it off
   the Fire Mountain summit: the human reaches y=4649 at frame 8700, hangs with
   all four wheels off the ground for 84 frames with its pitch ramping 3317 ->
   4133, never crosses the line, and `racer_boss_finish()` is never reached at
   ANY budget -- so the whole win half of this check went vacuous.  It now uses
   `MDKR_BOSS_WIN=1` (platform/mdkr_adventure.c), which writes the one field the
   verdict branches on, `racer->finishPosition`, once the race has finished
   normally.  The win arm's racing line is therefore identical to the losing
   arm's, and what is asserted -- finishPos 1 => WIN cutscene, finishPos 2 => LOSE
   cutscene -- is isolated to that single variable.

    MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py -v      # ~6 min

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import os
import re
import subprocess
import sys

from harness_utils import (DEFAULT_BUILD_DIR, preserved_eeprom, resolve_binary,
                           save_env, test_save_dir)

# Boss level 46 = Tricky 2, the Fire Mountain spiral. Decoded from the ROM's
# ASSET_MISC_BOSS_TRACKS_IDS with tools/dump_misc_asset.py (sub-asset 30):
# 38, 46, 40, 53, 1, 52, 41, 54, 37, 55.  38 and 46 are the two Tricky arenas and
# the only two levels measured to saturate the candidate list. Level 38's
# automated line became a knife-edge after object-model collision was restored;
# level 46 still reproduces the broken grid mask and finishes with production
# object collision, so it is the integrated fixture.
BOSS_TRACK = 46
# Ancient Lake -- the negative control. Never saturates, so the two arms must be
# bit-identical there.
CONTROL_TRACK = 5

SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
SAVE_DIR = test_save_dir()
SAVE = os.path.join(SAVE_DIR, "eeprom.bin")
# The fixed arm finishes at ~8600 and then loads the LOSE cutscene at 8965
# (autopilot races like the CPU field, so it finishes 2nd). The
# budget has to clear that load, or the "the lose cutscene never precedes the
# race" assertion would have nothing to order and would pass vacuously.
BOSS_FRAMES = 13000
CONTROL_FRAMES = 6500

# The broken arm must saturate. 500 is MAX_COLLISION_CANDIDATES (game/src/collision.h).
CAP = 500
MIN_BROKEN_TRUNCATIONS = 10      # measured 36 on the integrated level-46 fixture
# "This racer lost the ground": it stayed airborne far longer than the SAME run
# pair's fixed arm managed anywhere on the same level.
#
# There is deliberately NO absolute ceiling on the fixed arm's airborne run. One
# used to live here (MAX_FIXED_AIRBORNE = 45, against measured 18 boss / 27 human)
# and it was the same shape this file already rejected for the broken arm's peak
# y a few lines down: Fire Mountain's spiral has legitimate airborne stretches, so
# a route move that lands one real jump differently pushes a healthy run past the
# constant and fails the arm that is supposed to be the good one -- and because
# the broken arm's floor was derived from it, the same nudge moved both. What the
# check has to separate is "the grid mask drops terrain out from under racers"
# from "it does not", and that is a PROPERTY of the pair: the broken arm must lose
# the ground for a multiple of whatever the fixed arm does on the very same line.
# Measured ratio on the integrated fixture: boss 51/18 = 2.8x.
BROKEN_AIRBORNE_RATIO = 2.0
# Floor under the derived threshold, so a fixed arm that never leaves the ground
# at all cannot make a one-frame broken-arm hop count as "lost the ground".
MIN_BROKEN_AIRBORNE_FLOOR = 20
MIN_BROKEN_RACERS = 1
# The fixed arm climbs the whole spiral (measured 4647-4868).
#
# There is deliberately NO corresponding upper bound on the broken arm's peak y,
# even though it measures 2118 on this build. That bound was written and then
# removed: it encodes *where* the AI line happens to fall off, and while
# positive-controlling the cutscene assertions below (which lengthen the pre-race
# cutscene by ~136 frames and so shift the whole racing line) the broken arm
# reached the summit first and fell later -- still truncating 73 times and still
# losing the ground for 103 frames, i.e. still exhibiting the defect, but with a
# peak y of 4868. An assertion that fails on an unrelated route shift is not
# measuring the defect. The truncation count, the saturated cap, the sustained
# ground loss and the unfinished race are all route-independent, so they carry it.
MIN_FIXED_PEAK_Y = 3500

PACE_RE = re.compile(r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) "
                     r"clock=(\d+) cp=(\d+) lap=(\d+) rlap=(\d+) fin=(\d+)")
GRND_RE = re.compile(r"\[GRND\] frame=(\d+) pi=(-?\d+) ri=(-?\d+) gw=(\d+) surf=(\S+) "
                     r"xrot=(-?\d+) zrot=(-?\d+)")
BOSSFINISH_RE = re.compile(r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+)")
COLL_RE = re.compile(r"\[COLL\] maxCandidates=(\d+) truncated=(\d+) cap=(\d+)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")

# --- second report from the same session: "on first load of the first boss it
# went straight to the 'you lose' animation, then started the race" -----------
#
# A first boss entry is SUPPOSED to play a cutscene before the race: level_load()
# (game/src/game.c) redirects a not-yet-visited RACETYPE_BOSS level to a cutscene
# level via ASSET_MISC_67, races it with the real level pushed on the level-property
# stack, and pops back afterwards.  Which of the five cutscenes that level holds
# plays is chosen by gCutsceneID, which the animation objects' `channel` field is
# filtered against (objects.c func_8001E4C4).  For Tricky's cutscene level 57 the
# channels are, from racer_boss_finish()'s pushes in game/src/vehicle_tricky.c:
#   3 = challenge (first visit)   4 = you win    5 = YOU LOSE
#   6 = wizpig amulet             7 = rematch challenge
# level_load() can only ever select 3 or 7 -- both are literal constants -- so
# channel 5 before a race is unreachable there, and the only producer of it is
# racer_boss_finish() on a losing finish.  These two assertions pin that down:
# the first entry must pick 3, and any cutscene-5 load must come after the race.
BOSSREDIRECT_RE = re.compile(r"bossredirect: boss=(\d+) -> cutsceneLevel=(\d+) "
                             r"cutsceneId=(\d+) bosses=0x([0-9a-f]+) "
                             r"courseFlags=0x([0-9a-f]+) worldId=(-?\d+)")
LEVELLOAD_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+) "
                          r"entrance=(-?\d+) vehicle=(-?\d+) cutscene=(-?\d+) "
                          r"@frame~(\d+)")
CUTSCENE_CHALLENGE_FIRST = 3
CUTSCENE_WIN = 4
CUTSCENE_LOSE = 5
# The win path is longer than the lose path -- level_transition_begin(4) runs 360
# frames against 282, plus instShowBearBar() -- so the win arm needs a bigger
# budget than the lose arm to clear its cutscene load. Measured at 9139.
WIN_FRAMES = 13000


def run(binary, rom, script, track, frames, legacy, verbose, boss_win=False):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
    save_env(env, SAVE_DIR)
    env["MDKR_PRESENT_RATE"] = "original"
    env["MDKR_SIMULATION_CADENCE"] = "original"
    env["MDKR_SYNTH_FIELDS"] = "2"   # one authored gameplay ticket per opportunity
    env["MDKR_AUTOPILOT"] = "1"      # drive with DKR's own AI -- no per-track tuning
    env["MDKR_TRACE"] = "1"          # emit [PACE] and [GRND]
    env["MDKR_LOAD_TRACK"] = str(track)
    if legacy:
        env["MDKR_GRIDMASK"] = "off"
    else:
        env.pop("MDKR_GRIDMASK", None)
    # MDKR_BOSS_WIN awards the human first place at the finish; it does NOT touch
    # the physics, so the win arm drives the same line as the losing arm. It
    # replaced MDKR_BOSS_SLOW -- see the docstring -- which is why that variable is
    # actively cleared here rather than merely not set: an inherited
    # MDKR_BOSS_SLOW=1 would silently reintroduce the trajectory shift this check
    # was rewritten to get rid of.
    env.pop("MDKR_BOSS_SLOW", None)
    if boss_win:
        env["MDKR_BOSS_WIN"] = "1"
    else:
        env.pop("MDKR_BOSS_WIN", None)
    cmd = [binary, "--headless-frames", str(frames),
           "--input-script", script, "--rom", rom]
    if verbose:
        print("  $ MDKR_LOAD_TRACK=%d MDKR_GRIDMASK=%s MDKR_BOSS_WIN=%s %s" %
              (track, "off" if legacy else "(unset)", "1" if boss_win else "(unset)",
               " ".join(cmd)))
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=900)
    return proc.stdout.decode("utf-8", "replace"), proc.returncode


def parse(out, track):
    """-> dict of everything this check asserts on."""
    pace = [[float(g) for g in m.groups()] for m in PACE_RE.finditer(out)]
    grnd = [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in GRND_RE.finditer(out)]
    coll = COLL_RE.search(out)
    loads = [(int(m.group(1)), int(m.group(5)), int(m.group(6)))
             for m in LEVELLOAD_RE.finditer(out)]
    # The boss level loads twice: the cutscene redirect, then the race. Ground
    # loss is only meaningful from the race load onward -- before that comes the
    # pre-race grid settle, which both arms have and which is not a fall.
    boss_loads = [fr for lvl, _cid, fr in loads if lvl == track]
    race_from = boss_loads[1] if len(boss_loads) >= 2 else (boss_loads[0] if boss_loads else 0)
    # Longest run of "no wheel found a surface", PER RACER. Per-racer is
    # load-bearing: the reporter's decisive observation was that the AI boss
    # falls through even when the player does not, so a player-only measurement
    # can miss the defect entirely.
    cur: dict = {}
    longest: dict = {}
    for f, pi, ri, gw in grnd:
        if f < race_from:
            continue
        key = (pi, ri)
        cur[key] = cur.get(key, 0) + 1 if gw == 0 else 0
        if cur[key] > longest.get(key, 0):
            longest[key] = cur[key]
    return {
        "pace": pace,
        "grnd": grnd,
        "perRacerAirborne": longest,
        "raceFrom": race_from,
        "bossFinish": [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
                       for m in BOSSFINISH_RE.finditer(out)],
        "maxCandidates": int(coll.group(1)) if coll else None,
        "truncated": int(coll.group(2)) if coll else None,
        "cap": int(coll.group(3)) if coll else None,
        "airborne": max(longest.values(), default=0),
        "peakY": max((p[2] for p in pace), default=None),
        "fin": max((int(p[8]) for p in pace), default=None),
        "maxCp": max((int(p[5]) for p in pace), default=None),
        "stream": [tuple(p[1:4]) for p in pace],
        "bad": BAD_RE.findall(out),
        "redirects": [(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                       int(m.group(4), 16), int(m.group(5), 16))
                      for m in BOSSREDIRECT_RE.finditer(out)],
        "loads": [(int(m.group(1)), int(m.group(5)), int(m.group(6)))
                  for m in LEVELLOAD_RE.finditer(out)],
        "firstRaceFrame": int(pace[0][0]) if pace else None,
    }


def main() -> int:
    # This check deletes and rewrites EEPROM images to assert a known starting
    # state. Under tools/run_checks.py SAVE_DIR is a run-scoped temporary
    # directory; a standalone run defaults to the repository's playable save/,
    # whose prior contents must survive however this exits.
    with preserved_eeprom(SAVE_DIR):
        return run_check()


def run_check() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=SCRIPT)
    ap.add_argument("--track", type=int, default=BOSS_TRACK)
    ap.add_argument("--frames", type=int, default=BOSS_FRAMES)
    ap.add_argument("--control-track", type=int, default=CONTROL_TRACK)
    ap.add_argument("--control-frames", type=int, default=CONTROL_FRAMES)
    ap.add_argument("--win-frames", type=int, default=WIN_FRAMES)
    ap.add_argument("--skip-control", action="store_true",
                    help="skip the byte-identity negative control (halves the runtime)")
    ap.add_argument("--skip-win", action="store_true",
                    help="skip the MDKR_BOSS_WIN verdict arm")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, args.script):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    arms = {}
    for name, legacy in (("fixed", False), ("broken", True)):
        # A recorded time changes the menu route, and a save makes the boss entry
        # a REVISIT (no challenge cutscene) -- which would make the cutscene
        # assertions below vacuous. Same discipline as check_adventure_race_loop.py.
        if os.path.exists(SAVE):
            os.remove(SAVE)
        out, rc = run(binary, args.rom, args.script, args.track, args.frames,
                      legacy, args.verbose)
        r = parse(out, args.track)
        arms[name] = r
        if rc != 0:
            failures.append("boss arm %s: exit code %d" % (name, rc))
        if r["bad"]:
            failures.append("boss arm %s: %s in output" % (name, r["bad"][0]))
        # Fail closed: without the probes the assertions below would be vacuous.
        if r["maxCandidates"] is None:
            failures.append("boss arm %s: no '[COLL] maxCandidates=' line -- the "
                            "candidate-saturation probe is gone, so this check "
                            "cannot see the mechanism it exists to test" % name)
        if not r["grnd"]:
            failures.append("boss arm %s: no '[GRND]' rows -- the ground-contact "
                            "probe is gone" % name)
        if not r["pace"]:
            failures.append("boss arm %s: no '[PACE] ... racer' rows -- the route "
                            "never reached a race" % name)
        if args.verbose:
            print("  arm %-6s maxCandidates=%s truncated=%s airborne=%d peakY=%s "
                  "maxCp=%s fin=%s"
                  % (name, r["maxCandidates"], r["truncated"], r["airborne"],
                     None if r["peakY"] is None else round(r["peakY"]),
                     r["maxCp"], r["fin"]))
    if failures:
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        print("check_collision_gridmask: FAIL")
        return 1

    b, f = arms["broken"], arms["fixed"]

    # --- the positive control must actually be broken -----------------------
    # If MDKR_GRIDMASK=off stops reproducing the defect, everything below is
    # unfalsifiable, so these are assertions and not diagnostics.
    if b["truncated"] < MIN_BROKEN_TRUNCATIONS:
        failures.append("MDKR_GRIDMASK=off truncated the candidate list only %d "
                        "time(s) (want >= %d) -- the positive control no longer "
                        "reproduces the defect, so this check proves nothing"
                        % (b["truncated"], MIN_BROKEN_TRUNCATIONS))
    if b["maxCandidates"] < CAP:
        failures.append("MDKR_GRIDMASK=off peaked at %d candidates, never reaching "
                        "the %d cap -- positive control not reproduced"
                        % (b["maxCandidates"], CAP))
    # "Lost the ground" is derived PER RACER from the SAME run pair, not from a
    # constant: this racer stayed airborne at least BROKEN_AIRBORNE_RATIO times as
    # long in the broken arm as it did in the fixed arm on the same line, and for
    # at least MIN_BROKEN_AIRBORNE_FLOOR frames so a one-frame hop against a
    # never-airborne fixed run cannot qualify. See the docstring for why the old
    # hard-coded 60 was a trajectory constant in disguise, and the constants block
    # for why its replacement (a fixed 45/46 pair) was the same shape.
    def lost_ground_threshold(key):
        return max(int(BROKEN_AIRBORNE_RATIO * f["perRacerAirborne"].get(key, 0)) + 1,
                   MIN_BROKEN_AIRBORNE_FLOOR)

    broken_racers = sorted(k for k, v in b["perRacerAirborne"].items()
                           if v >= lost_ground_threshold(k))
    # At least one racer must lose the ground. Requiring both encoded the exact
    # level-38 racing lines: after production object collision was restored,
    # level 46's broken grid mask still saturates and drops terrain under the boss
    # while the human takes a different line. The mechanism is established by
    # saturation/truncation plus this paired fixed-arm comparison, not by a
    # particular number of racers following the same bad line.
    if len(broken_racers) < MIN_BROKEN_RACERS:
        failures.append("MDKR_GRIDMASK=off: no racer lost the ground for >= %.1fx "
                        "its own fixed-arm run (and >= %d frames), want >= %d "
                        "racer(s). Per-racer longest gw==0 runs: broken=%s "
                        "fixed=%s"
                        % (BROKEN_AIRBORNE_RATIO, MIN_BROKEN_AIRBORNE_FLOOR,
                           MIN_BROKEN_RACERS,
                           dict(sorted(b["perRacerAirborne"].items())),
                           dict(sorted(f["perRacerAirborne"].items()))))
    # --- the fix must avoid it ---------------------------------------------
    if f["truncated"] != 0:
        failures.append("fixed arm truncated the candidate list %d time(s) (want 0) "
                        "-- terrain is still being discarded, so a racer can still "
                        "end up standing on nothing" % f["truncated"])
    if f["maxCandidates"] >= CAP:
        failures.append("fixed arm peaked at %d candidates, at or above the %d cap"
                        % (f["maxCandidates"], CAP))
    if f["peakY"] < MIN_FIXED_PEAK_Y:
        failures.append("fixed arm only climbed to y=%.0f (want >= %d) -- it did not "
                        "get up the spiral" % (f["peakY"], MIN_FIXED_PEAK_Y))
    # The gameplay-level consequence, and the one a player would report: with the
    # defect the first boss race cannot be finished at all.
    if f["fin"] != 1:
        failures.append("fixed arm never reached fin=1 -- the boss race did not "
                        "finish, which is the whole point of the fix")
    if b["fin"] == 1:
        failures.append("MDKR_GRIDMASK=off ALSO finished the race (fin=1) -- the "
                        "control is not reproducing the defect")

    # --- the boss cutscene the FIRST entry selects --------------------------
    # Asserted on the fixed arm only; the run starts from a deleted eeprom.bin, so
    # this really is a first visit (bosses == 0, courseFlags == 0).
    if not f["redirects"]:
        failures.append("no 'bossredirect:' line -- level_load() did not treat "
                        "level %d as an unvisited RACETYPE_BOSS, so the pre-race "
                        "cutscene selection is not being exercised" % args.track)
    else:
        boss, cutLevel, cutId, bosses, courseFlags = f["redirects"][0]
        if cutId != CUTSCENE_CHALLENGE_FIRST:
            failures.append("first entry to boss %d selected cutsceneId=%d, want %d "
                            "(the challenge). bosses=0x%x courseFlags=0x%x -- %d is "
                            "the LOSE cutscene and must never precede a race"
                            % (boss, cutId, CUTSCENE_CHALLENGE_FIRST, bosses,
                               courseFlags, CUTSCENE_LOSE))
        if bosses != 0 or courseFlags != 0:
            failures.append("first entry to boss %d saw bosses=0x%x courseFlags=0x%x, "
                            "want 0/0 -- delete save/eeprom.bin; with a save present "
                            "this is not a first visit and the assertion above is "
                            "meaningless" % (boss, bosses, courseFlags))
        if args.verbose:
            print("  bossredirect: boss=%d -> cutsceneLevel=%d cutsceneId=%d "
                  "(3 == challenge)" % (boss, cutLevel, cutId))
    # The boss level is loaded twice: once for the redirect-to-cutscene (which
    # level_load rewrites internally) and once for the race proper, after the
    # level-property stack pops. The second one is the race.
    boss_loads = [fr for lvl, _cid, fr in f["loads"] if lvl == args.track]
    lose_loads = [fr for _lvl, cid, fr in f["loads"] if cid == CUTSCENE_LOSE]
    if len(boss_loads) < 2:
        failures.append("boss level %d was loaded %d time(s), want 2 (the cutscene "
                        "redirect, then the race) -- the boss cutscene flow is not "
                        "being exercised" % (args.track, len(boss_loads)))
    else:
        race_frame = boss_loads[1]
        early = [fr for fr in lose_loads if fr <= race_frame]
        if early:
            failures.append("the LOSE cutscene (cutscene=%d) was loaded at frame %d, "
                            "at or BEFORE the boss race load at frame %d"
                            % (CUTSCENE_LOSE, early[0], race_frame))
        if not lose_loads:
            # Without one, the ordering assertion above has nothing to order.
            failures.append("the LOSE cutscene (cutscene=%d) was never loaded -- "
                            "autopilot finishes mid-field, so a losing boss finish "
                            "should reach racer_boss_finish()'s lose branch. Either "
                            "the budget (%d frames) no longer clears it or the "
                            "post-race boss flow broke, and the ordering assertion "
                            "above is now vacuous" % (CUTSCENE_LOSE, args.frames))
        elif args.verbose:
            print("  boss loads at %s (race = %d); lose-cutscene loads at %s -- after"
                  % (boss_loads, race_frame, lose_loads))

    # --- the boss VERDICT must follow the finish, in both directions ---------
    # "I was awarded first place, still got the failure animation and was returned
    # without having won." racer_boss_finish() branches on racer->finishPosition,
    # so the falsifiable statement is: finishPos == 2 => the LOSE cutscene, and
    # finishPos == 1 => the WIN cutscene. The lose direction is the arm above
    # (autopilot always comes second); MDKR_BOSS_WIN=1 supplies the other by
    # writing finishPosition at the finish and nothing else, so the two arms drive
    # the same line.
    if not f["bossFinish"]:
        failures.append("no 'bossfinish:' line -- racer_boss_finish() never ran, so "
                         "the boss result is not being exercised")
    elif f["bossFinish"][0][0] != 2:
        failures.append("losing arm: racer_boss_finish() read finishPos=%d, want 2. "
                        "Autopilot drives the human with the boss's own AI and comes "
                        "second, so anything else means finishPosition does not "
                        "follow the actual finish" % f["bossFinish"][0][0])
    if not args.skip_win:
        if os.path.exists(SAVE):
            os.remove(SAVE)
        out, rc = run(binary, args.rom, args.script, args.track, args.win_frames,
                      False, args.verbose, boss_win=True)
        w = parse(out, args.track)
        if rc != 0:
            failures.append("win arm: exit code %d" % rc)
        if w["bad"]:
            failures.append("win arm: %s in output" % w["bad"][0])
        if not w["bossFinish"]:
            failures.append("win arm: no 'bossfinish:' line -- the race did not reach "
                            "racer_boss_finish() within %d frames, so the win branch "
                            "is unexercised and the assertion below is vacuous"
                            % args.win_frames)
        else:
            pos = w["bossFinish"][0][0]
            if pos != 1:
                failures.append("win arm: racer_boss_finish() read finishPos=%d, want "
                                "1 -- MDKR_BOSS_WIN=1 writes finishPosition at the "
                                "finish, so either the hook did not fire or something "
                                "overwrote it afterwards" % pos)
            win_loads = [fr for _lvl, cid, fr in w["loads"] if cid == CUTSCENE_WIN]
            bad_loads = [fr for _lvl, cid, fr in w["loads"] if cid == CUTSCENE_LOSE]
            if not win_loads:
                failures.append("win arm: the WIN cutscene (cutscene=%d) was never "
                                "loaded after a first-place boss finish" % CUTSCENE_WIN)
            if bad_loads:
                failures.append("win arm: the LOSE cutscene (cutscene=%d) was loaded "
                                "at frame %d after a FIRST-PLACE finish -- the boss "
                                "verdict disagrees with the race result"
                                % (CUTSCENE_LOSE, bad_loads[0]))
            if args.verbose:
                print("  win arm: finishPos=%d, win-cutscene loads at %s, "
                      "lose-cutscene loads at %s" % (pos, win_loads, bad_loads or "none"))

    # --- negative control: nothing else may move ---------------------------
    # Where the list never truncates, an over-permissive mask and a correct one
    # must select exactly the same colliding facets, so the simulation has to be
    # bit-identical. Anything else means the fix changed a collision it should
    # not have.
    if not args.skip_control:
        ctl = {}
        for name, legacy in (("fixed", False), ("broken", True)):
            out, rc = run(binary, args.rom, args.script, args.control_track,
                          args.control_frames, legacy, args.verbose)
            ctl[name] = parse(out, args.control_track)
            if rc != 0:
                failures.append("control arm %s: exit code %d" % (name, rc))
            if ctl[name]["bad"]:
                failures.append("control arm %s: %s" % (name, ctl[name]["bad"][0]))
        cb, cf = ctl["broken"], ctl["fixed"]
        if not cf["stream"]:
            failures.append("control track %d produced no racer rows" % args.control_track)
        elif cb["truncated"] or cf["truncated"]:
            failures.append("control track %d truncated the candidate list "
                            "(broken=%s fixed=%s) -- it is no longer a valid "
                            "neutrality control; pick a track that does not saturate"
                            % (args.control_track, cb["truncated"], cf["truncated"]))
        elif cb["stream"] != cf["stream"]:
            n = sum(1 for x, y in zip(cb["stream"], cf["stream"]) if x != y)
            failures.append("control track %d: the two arms diverge in %d of %d "
                            "racer rows. With no truncation the fix must be "
                            "behaviour-neutral"
                            % (args.control_track, n, len(cf["stream"])))
        elif args.verbose:
            print("  control track %d: %d racer rows byte-identical across both arms"
                  % (args.control_track, len(cf["stream"])))

    # Leave no save behind: a recorded time changes every other fixture's route.
    if os.path.exists(SAVE):
        os.remove(SAVE)
    if failures:
        for msg in failures:
            print("  - %s" % msg, file=sys.stderr)
        print("check_collision_gridmask: FAIL")
        return 1
    print("check_collision_gridmask: PASS  (boss %d: broken truncated=%d airborne=%d "
          "peakY=%.0f fin=%s | fixed truncated=%d airborne=%d peakY=%.0f fin=%s)"
          % (args.track, b["truncated"], b["airborne"], b["peakY"], b["fin"],
             f["truncated"], f["airborne"], f["peakY"], f["fin"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
