#!/usr/bin/env python3
"""A first boss WIN must present its win cutscene -- and the one state that can
silently cancel it must not be set beforehand.

The bug this locks down
-----------------------
`racer_boss_finish()` (game/src/vehicle_tricky.c) chooses between "play the win
cutscene and award the amulet" and "just transition back to the overworld" by
testing ONE bit:

    if (settings->courseFlagsPtr[settings->courseId] & 2) {   /* RACE_CLEARED */
        if (finishPos == 1) { level_transition_begin(4); instShowBearBar(); }
        else                { level_transition_begin(3); ... }
        arg1_ret++; *sceneTimer = arg1_ret; return;            /* <-- no push */
    }

That branch pushes NOTHING onto the level-property stack, so no cutscene level is
ever queued, no amulet is awarded and `safe_mark_write_save_file()` is not
reached: the player is simply returned to the world lobby, standing at the boss
door.  It is the ROM's own "you have already beaten this boss" path -- correct on
a re-race, and indistinguishable from a broken first win.

RACE_CLEARED on a BOSS course is written in exactly one place: the win branch of
that same function, together with `settings->bosses |= 1 << worldId`.  So a first
boss win presents correctly IF AND ONLY IF nothing has set either bit
beforehand.  That is not a safe assumption to leave unmeasured -- both fields are
written by index from unrelated subsystems (golden balloons, doors, triggers and
the save unpacker all do `courseFlagsPtr[<index>] |= ...`, and the save packs
`bosses` as 12 loose bits), so ONE stray index sets RACE_CLEARED on a boss
course, and from then on every first boss win loses its cutscene and its amulet
with no crash, no assertion and nothing in the log.  That is the exact shape of
the reported defect:

    "I won the first boss race, got no win cutscene, no amulet piece, and was
     dropped back in front of the boss door as if it had just been unlocked."

Reported with this probe line from the live browser build:

    bossfinish: finishPos=1 playerIndex=0 courseId=38 worldId=1 bosses=0x2
                courseFlags=0x440003 cutsceneLevel=57 timer=1 raceFinished=1

`bosses=0x2` is `1 << worldId` for world 1 and `courseFlags & 2` is RACE_CLEARED:
that state can only come from an EARLIER execution of the win branch, so on that
save the boss had already been beaten and the skip was correct.  The "cleared"
arm below reproduces that line field-for-field from an unmodified binary
(`courseFlags` differs only in an arena trigger bit, 0x400000 vs 0x040000).

The other half of the report, and why it needed its own assertion
-----------------------------------------------------------------
The sentence the player actually wrote was "I was awarded first place, still got
the FAILURE ANIMATION and was returned without having won" (the probe line in
vehicle_tricky.c quotes it).  Two separate presentations are wrong in that
sentence: the win cutscene did not play, and the LOSE cutscene did.  Until the
"decisions" wave this check only asserted the first half -- cutscene 4 loads --
and said nothing at all about cutscene 5.  Those are not the same statement.
`racer_boss_finish()` reaches the lose animation from its own `else` limb:

    } else {                                       /* finishPos != 1 */
        level_properties_push(SPECIAL_MAP_ID_UNK_NEG10, 0, VEHICLE_CAR, 0);
        level_properties_push(i, 5, -1, 5);        /* <-- cutscene 5, the loss */
        level_transition_begin(3);
    }

`finishPos` is read ONCE, into a local, several statements before either limb
runs, and every branch after that reads the local.  So the branch is chosen by
one value -- and any writer that lands on `racer->finishPosition` between the
natural finish and this read flips a genuine first place onto the failure
animation with nothing else changing.  A build that pushed BOTH (cutscene 4 from
one path, cutscene 5 from another, on the same finish) passed the old
assertions, because "the win cutscene loads" was satisfied.  The winning arm now
also asserts that cutscene 5 never loads, which is the reported symptom stated
directly rather than inferred.

How the three arms work
-----------------------
All three are the SAME binary, all muted + headless:

  first    fresh save/eeprom.bin -> a genuine first visit.  Asserts the
           invariant (`bosses` and RACE_CLEARED both clear when
           racer_boss_finish reads them), that the win branch then sets BOTH in
           the same frame, that cutscene 4 really loads, and that cutscene 5 --
           the failure animation -- never loads at any point in the run.
  cleared  MDKR_BOSS_PRECLEARED=46 holds the fixture's boss course at "already beaten"
           (platform/mdkr_adventure.c, no-op unless set).  This is the POSITIVE
           CONTROL: it must reproduce the report -- no pre-race cutscene, no
           post-race cutscene at all -- because that is what proves the
           invariant asserted in the first arm is load-bearing rather than
           decorative.
  lose     the same first visit with MDKR_BOSS_WIN unset, so the human finishes
           where the autopilot naturally puts it (second) and DKR takes its own
           `else` limb.  This is the NON-VACUITY CONTROL for the new negative
           assertion: cutscene 5 MUST load here.  Without it, "cutscene 5 never
           loaded" is a claim about a log line no arm has ever been shown to
           produce, which is indistinguishable from a typo in the regex.
           Measured on us.v80: `level_load: levelId=57 numPlayers=1 entrance=5
           vehicle=-1 cutscene=5` -- the same LEVELLOAD_RE the winning arm reads,
           differing only in the two fields the assertion tests.

`MDKR_BOSS_WIN=1` is what lets the human win at all headlessly (autopilot drives
the player with the boss's own AI and otherwise always comes second): it writes
`racer->finishPosition = 1` once DKR has finished the race normally, leaving the
physics and the racing line untouched.  It replaced `MDKR_BOSS_SLOW=1`, which
reached the same branch by scaling the boss's velocity to 0.15x and, because the
AI paths relative to the field, moved the human's line too -- off the Fire
Mountain summit, once the ROM-faithful math landed.  See
tests/check_collision_gridmask.py's docstring for the measurement.
`MDKR_WATCH_COURSEFLAGS=46` emits one `[BOSSW]` line per change to
`courseFlagsPtr[46]` / `settings->bosses`.  The watcher POLLS at the frame
boundary instead of hooking each writer, deliberately: it therefore catches a
write through a wrong index just as well as a write through the intended one.

The report itself came from boss 38, and its probe line is preserved above.
The automated contract now runs on boss 46, which shares Tricky's world and
cutscene map: restoring object-model collision made the old boss-38 autopilot
line miss the summit after nine legitimate hits, while boss 46 finishes with
production collision enabled. The verdict invariant is boss-generic; changing
the route does not change the state transition being asserted.

    MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py -v          # ~3 min
    MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py --break-invariant

The second form is the both-directions proof: it sets MDKR_BOSS_PRECLEARED in
the FIRST arm too, i.e. it simulates exactly the regression this check exists to
catch, and the check must then FAIL.

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import os
import re
import subprocess
import sys

from harness_utils import (DEFAULT_BUILD_DIR, preserved_eeprom, resolve_binary,
                           save_env, test_save_dir)

# Boss level 46 = Tricky 2 (Fire Mountain), world 1. Same provenance as
# tests/check_collision_gridmask.py -- from ASSET_MISC_BOSS_TRACKS_IDS. The
# original level-38 fixture became a knife-edge after object-model collision was
# restored: nine legitimate hits move the automated line 2.5 units and it falls
# much later. Level 46 exercises the same verdict and cutscene contracts while
# finishing with production collision enabled.
BOSS_TRACK = 46
CUTSCENE_LEVEL = 57      # ASSET_MISC_67 maps both Tricky bosses -> cutscene level 57
CUTSCENE_WIN = 4         # channel 4 of level 57; 3 = challenge, 5 = lose, 6 = amulet
CUTSCENE_LOSE = 5        # the failure animation -- must never follow a 1st place
RACE_CLEARED = 2         # menu.h RACE_CLEARED (bit 1 of a courseFlags entry)

SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
SAVE_DIR = test_save_dir()
SAVE = os.path.join(SAVE_DIR, "eeprom.bin")
# Measured on us.v80: the win branch runs at frame 8685 and the win cutscene
# loads at 9043 -- level_transition_begin(4) takes ~360 frames -- so the budget
# has to clear 9043 or the "the win cutscene loads"
# assertion has nothing to see.
FRAMES = 13000

BOSSFINISH_RE = re.compile(
    r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+) worldId=(-?\d+) "
    r"bosses=0x([0-9a-f]+) courseFlags=0x([0-9a-f]+) cutsceneLevel=(-?\d+) "
    r"timer=(-?\d+) raceFinished=(-?\d+)")
BOSSREDIRECT_RE = re.compile(r"bossredirect: boss=(\d+) -> cutsceneLevel=(\d+) cutsceneId=(\d+)")
BOSSW_RE = re.compile(
    r"\[BOSSW\] frame=(\d+) courseFlags\[(\d+)\]=0x([0-9a-f]+) \(was 0x([0-9a-f]+)\) "
    r"bosses=0x([0-9a-f]+) \(was 0x([0-9a-f]+)\)")
LEVELLOAD_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+) vehicle=(-?\d+) "
    r"cutscene=(-?\d+) @frame~(\d+)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")


def run(binary, rom, track, frames, precleared, verbose, boss_win=True):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
    save_env(env, SAVE_DIR)
    env["MDKR_TRACE"] = "1"
    env["MDKR_AUTOPILOT"] = "1"
    # Award the human first place by writing racer->finishPosition at the finish
    # (platform/mdkr_adventure.c). This replaced MDKR_BOSS_SLOW=1, which reached
    # the win branch by scaling the BOSS's velocity to 0.15x: because the human is
    # driven by DKR's own AI and the AI paths relative to the field, that moved the
    # HUMAN's racing line as well, and once the ROM's RNG seed / arctan table /
    # table-based trig became the defaults it moved it off the Fire Mountain summit
    # -- 84 frames with all four wheels off the ground, no finish, and
    # racer_boss_finish() never reached at any budget. The slowdown is cleared
    # rather than merely unset so an inherited MDKR_BOSS_SLOW=1 cannot reintroduce
    # it. See tests/check_collision_gridmask.py's docstring.
    env.pop("MDKR_BOSS_SLOW", None)
    # The "lose" arm leaves this unset so the autopilot finishes where it
    # naturally does (second) and DKR takes its own finishPos != 1 limb. That is
    # the only arm in which cutscene 5 is SUPPOSED to load, and it is what makes
    # the winning arm's "cutscene 5 never loads" a falsifiable statement.
    if boss_win:
        env["MDKR_BOSS_WIN"] = "1"
    else:
        env.pop("MDKR_BOSS_WIN", None)
    env["MDKR_LOAD_TRACK"] = str(track)
    env["MDKR_WATCH_COURSEFLAGS"] = str(track)
    if precleared:
        env["MDKR_BOSS_PRECLEARED"] = str(track)
    else:
        env.pop("MDKR_BOSS_PRECLEARED", None)
    cmd = [binary, "--headless-frames", str(frames),
           "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print("  $ MDKR_LOAD_TRACK=%d MDKR_BOSS_PRECLEARED=%s MDKR_BOSS_WIN=%s %s"
              % (track, str(track) if precleared else "(unset)",
                 "1" if boss_win else "(unset)", " ".join(cmd)))
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=900)
    return proc.stdout.decode("utf-8", "replace"), proc.returncode


def parse(out):
    finish = BOSSFINISH_RE.search(out)
    return {
        "finish": None if finish is None else {
            "finishPos": int(finish.group(1)),
            "courseId": int(finish.group(3)),
            "worldId": int(finish.group(4)),
            "bosses": int(finish.group(5), 16),
            "courseFlags": int(finish.group(6), 16),
            "cutsceneLevel": int(finish.group(7)),
            "timer": int(finish.group(8)),
            "raceFinished": int(finish.group(9)),
            "line": finish.group(0),
        },
        "redirects": [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
                      for m in BOSSREDIRECT_RE.finditer(out)],
        "watch": [(int(m.group(1)), int(m.group(3), 16), int(m.group(5), 16))
                  for m in BOSSW_RE.finditer(out)],
        "loads": [(int(m.group(1)), int(m.group(3)), int(m.group(5)), int(m.group(6)))
                  for m in LEVELLOAD_RE.finditer(out)],
        "bad": BAD_RE.findall(out),
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
    ap.add_argument("--track", type=int, default=BOSS_TRACK)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--break-invariant", action="store_true",
                    help="set MDKR_BOSS_PRECLEARED in the FIRST arm too, simulating the "
                         "regression this check exists to catch. The check MUST then fail.")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    arms = {}
    for name, precleared, boss_win in (("first", args.break_invariant, True),
                                       ("cleared", True, True),
                                       ("lose", False, False)):
        # A save present makes the boss entry a REVISIT, which is the very state
        # the first arm must NOT be in. Same discipline as check_collision_gridmask.
        if os.path.exists(SAVE):
            os.remove(SAVE)
        out, rc = run(binary, args.rom, args.track, args.frames, precleared,
                      args.verbose, boss_win=boss_win)
        r = arms[name] = parse(out)
        if rc != 0:
            failures.append("arm %s: exit code %d" % (name, rc))
        if r["bad"]:
            failures.append("arm %s: %s in output" % (name, r["bad"][0]))
        # Fail closed: with no probe line every assertion below is vacuous.
        if r["finish"] is None:
            failures.append("arm %s: no 'bossfinish:' line -- racer_boss_finish() never "
                            "ran, so the boss verdict is not being exercised at all"
                            % name)
        elif boss_win and r["finish"]["finishPos"] != 1:
            failures.append("arm %s: racer_boss_finish() read finishPos=%d, want 1. "
                            "MDKR_BOSS_WIN=1 writes finishPosition at the finish, so "
                            "either the hook did not fire or something overwrote it; "
                            "without a WIN this check tests nothing"
                            % (name, r["finish"]["finishPos"]))
        elif not boss_win and r["finish"]["finishPos"] == 1:
            failures.append("arm %s: racer_boss_finish() read finishPos=1 with "
                            "MDKR_BOSS_WIN unset. This arm exists to take DKR's "
                            "LOSING limb; a win here means it proves nothing about "
                            "the failure animation" % name)
        if boss_win and not r["watch"]:
            failures.append("arm %s: no '[BOSSW]' lines -- MDKR_WATCH_COURSEFLAGS is not "
                            "reporting, so nothing can be said about when the flags moved"
                            % name)
        if args.verbose and r["finish"] is not None:
            print("  arm %-8s %s" % (name, r["finish"]["line"]))
    # Leave no save behind on ANY exit path: a recorded time changes the menu route
    # of every other fixture.
    if os.path.exists(SAVE):
        os.remove(SAVE)
    if failures:
        return report(failures)

    f, c = arms["first"], arms["cleared"]
    ff, cf = f["finish"], c["finish"]
    world_bit = 1 << ff["worldId"]

    # --- the invariant: nothing may pre-set the two bits the verdict reads -----
    if ff["courseFlags"] & RACE_CLEARED:
        failures.append("FIRST WIN: racer_boss_finish() read courseFlags=0x%x on boss %d, "
                        "which already has RACE_CLEARED (0x2) set. That takes the "
                        "'already beaten' branch (vehicle_tricky.c), which pushes NO "
                        "cutscene level and awards nothing -- every first boss win now "
                        "silently loses its win cutscene and its amulet. RACE_CLEARED on a "
                        "boss course must be written by nothing except this function's own "
                        "win branch; find the writer that got there first"
                        % (ff["courseFlags"], ff["courseId"]))
    if ff["bosses"] & world_bit:
        failures.append("FIRST WIN: racer_boss_finish() read bosses=0x%x with world %d's bit "
                        "(0x%x) already set, so the win branch takes the amulet/rematch arm "
                        "instead of the first-win arm. Only this function's win branch and "
                        "the save unpacker write settings->bosses"
                        % (ff["bosses"], ff["worldId"], world_bit))
    if ff["timer"] != 1:
        failures.append("FIRST WIN: the boss cutscene timer was %d, not 1 -- "
                        "racer_boss_finish() only does anything on the frame the latch "
                        "flips, so this call did nothing and the arm proves nothing"
                        % ff["timer"])
    if ff["raceFinished"] != 1:
        failures.append("FIRST WIN: raceFinished=%d at the verdict" % ff["raceFinished"])

    # The win branch sets courseFlags |= 2 and bosses |= worldBit within three
    # statements of each other, so they must appear in the SAME observed frame.
    # RACE_CLEARED arriving alone is the signature of a stray writer.
    cleared_frames = [fr for fr, flags, _b in f["watch"] if flags & RACE_CLEARED]
    bosses_frames = [fr for fr, _fl, b in f["watch"] if b & world_bit]
    if not cleared_frames:
        failures.append("FIRST WIN: courseFlags[%d] never gained RACE_CLEARED -- the win was "
                        "not recorded at all, so re-racing the boss would offer the "
                        "challenge again" % ff["courseId"])
    elif not bosses_frames:
        failures.append("FIRST WIN: courseFlags[%d] gained RACE_CLEARED at frame %d but "
                        "settings->bosses never gained world %d's bit. The win branch sets "
                        "both; RACE_CLEARED alone means something OTHER than "
                        "racer_boss_finish() wrote it"
                        % (ff["courseId"], cleared_frames[0], ff["worldId"]))
    elif cleared_frames[0] != bosses_frames[0]:
        failures.append("FIRST WIN: RACE_CLEARED appeared at frame %d but the boss bit at "
                        "frame %d. The win branch writes both in the same call, so they "
                        "cannot be observed on different frames -- one of them has another "
                        "writer" % (cleared_frames[0], bosses_frames[0]))

    # --- and the presentation must actually happen ---------------------------
    win_loads = [fr for lvl, en, cs, fr in f["loads"]
                 if lvl == CUTSCENE_LEVEL and cs == CUTSCENE_WIN and en == CUTSCENE_WIN]
    if not win_loads:
        cut_loads = [(en, cs, fr) for lvl, en, cs, fr in f["loads"] if lvl == CUTSCENE_LEVEL]
        failures.append("FIRST WIN: the win cutscene (levelId=%d entrance=%d cutscene=%d) "
                        "never loaded. Loads of cutscene level %d: %s. Last levels loaded: %s"
                        % (CUTSCENE_LEVEL, CUTSCENE_WIN, CUTSCENE_WIN, CUTSCENE_LEVEL,
                           cut_loads or "none",
                           [(lvl, fr) for lvl, _en, _cs, fr in f["loads"][-4:]]))
    elif cleared_frames and win_loads[0] < cleared_frames[0]:
        failures.append("FIRST WIN: a cutscene-%d load at frame %d PRECEDES the verdict at "
                        "frame %d" % (CUTSCENE_WIN, win_loads[0], cleared_frames[0]))
    # ...and the presentation that must NOT happen. This is the reported symptom
    # stated directly: "I was awarded first place, still got the failure
    # animation". racer_boss_finish() reaches cutscene 5 only through its
    # finishPos != 1 limb, so a cutscene-5 load anywhere in a run whose verdict
    # read finishPos == 1 means something re-entered the verdict with a
    # different position, or pushed the loss channel from outside it. Either way
    # the player sees the wrong animation after winning. Both fields are tested
    # because the loss is pushed as level_properties_push(i, 5, -1, 5) -- a
    # regression that corrupts only one of them is still the reported bug.
    lose_loads = [(en, cs, fr) for lvl, en, cs, fr in f["loads"]
                  if lvl == CUTSCENE_LEVEL and CUTSCENE_LOSE in (en, cs)]
    if lose_loads:
        failures.append("FIRST WIN: the LOSE cutscene (levelId=%d channel %d) loaded %s on a "
                        "finish that racer_boss_finish() read as finishPos=1. That is the "
                        "reported symptom -- first place, failure animation. The loss channel "
                        "is pushed only from the finishPos != 1 limb of racer_boss_finish() "
                        "(vehicle_tricky.c), so find what re-read or rewrote finishPosition "
                        "after the verdict, or what pushed the level property from outside it"
                        % (CUTSCENE_LEVEL, CUTSCENE_LOSE, lose_loads))
    if not f["redirects"]:
        failures.append("FIRST WIN: no 'bossredirect:' line -- level_load() did not treat "
                        "boss %d as a first visit, so this arm is not a first visit and "
                        "the invariant above was tested against the wrong state (delete "
                        "save/eeprom.bin)" % args.track)

    # --- positive control: the reported failure must reproduce ---------------
    # If it does not, the invariant asserted above is unfalsifiable here.
    if not (cf["courseFlags"] & RACE_CLEARED) or not (cf["bosses"] & (1 << cf["worldId"])):
        failures.append("CONTROL: MDKR_BOSS_PRECLEARED=%d did not hold the boss course at "
                        "'already beaten' (bosses=0x%x courseFlags=0x%x) -- the control is "
                        "not reproducing, so the first arm's assertions prove nothing"
                        % (args.track, cf["bosses"], cf["courseFlags"]))
    ctl_cutscenes = [(en, cs, fr) for lvl, en, cs, fr in c["loads"] if lvl == CUTSCENE_LEVEL]
    if ctl_cutscenes:
        failures.append("CONTROL: with the boss course already cleared, cutscene level %d "
                        "still loaded %s -- the 'already beaten' branch is supposed to push "
                        "nothing at all, so this check can no longer tell a skipped "
                        "presentation from a working one"
                        % (CUTSCENE_LEVEL, ctl_cutscenes))
    if c["redirects"]:
        failures.append("CONTROL: a 'bossredirect:' line appeared even though "
                        "courseFlags[%d] & RACE_VISITED was already set -- level_load() "
                        "should not offer a pre-race cutscene on a revisit"
                        % args.track)

    # --- non-vacuity control for the negative assertion above ----------------
    # "Cutscene 5 never loaded" is only evidence if a cutscene-5 load is
    # something this harness can actually see. The losing arm makes it produce
    # one, through the same LEVELLOAD_RE, in the same binary, on the same track.
    lo = arms["lose"]
    lose_control = [(en, cs, fr) for lvl, en, cs, fr in lo["loads"]
                    if lvl == CUTSCENE_LEVEL and cs == CUTSCENE_LOSE]
    if not lose_control:
        cut_loads = [(en, cs, fr) for lvl, en, cs, fr in lo["loads"] if lvl == CUTSCENE_LEVEL]
        failures.append("CONTROL: with MDKR_BOSS_WIN unset the human finished %s and DKR's "
                        "losing limb should have pushed cutscene %d of level %d, but no such "
                        "load was traced (cutscene-level loads seen: %s). The winning arm's "
                        "'the failure animation never loads' therefore asserts nothing -- it "
                        "is reading for a line this harness has never been shown to emit"
                        % ("finishPos=%d" % lo["finish"]["finishPos"]
                           if lo["finish"] else "(no verdict)",
                           CUTSCENE_LOSE, CUTSCENE_LEVEL, cut_loads or "none"))

    if args.verbose:
        print("  first   watch: %s" % [(fr, hex(fl), hex(b)) for fr, fl, b in f["watch"]])
        print("  first   loads after verdict: %s"
              % [(lvl, en, cs, fr) for lvl, en, cs, fr in f["loads"]
                 if cleared_frames and fr >= cleared_frames[0]])
        print("  cleared watch: %s" % [(fr, hex(fl), hex(b)) for fr, fl, b in c["watch"]])
        print("  cleared loads after verdict: %s"
              % [(lvl, en, cs, fr) for lvl, en, cs, fr in c["loads"] if fr > 8000])
        print("  lose    cutscene-level loads: %s"
              % [(en, cs, fr) for lvl, en, cs, fr in lo["loads"] if lvl == CUTSCENE_LEVEL])

    if failures:
        return report(failures)
    print("check_boss_win_verdict: PASS  (first win: bosses=0x%x courseFlags=0x%x -> "
          "cutscene %d @%d, cutscene %d absent; already beaten: bosses=0x%x "
          "courseFlags=0x%x -> no cutscene; losing finish -> cutscene %d @%d)"
          % (ff["bosses"], ff["courseFlags"], CUTSCENE_WIN, win_loads[0] if win_loads else -1,
             CUTSCENE_LOSE, cf["bosses"], cf["courseFlags"],
             CUTSCENE_LOSE, lose_control[0][2] if lose_control else -1))
    return 0


def report(failures) -> int:
    for msg in failures:
        print("  - %s" % msg, file=sys.stderr)
    print("check_boss_win_verdict: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
