#!/usr/bin/env python3
"""CPU opponents must always get their stuck-recovery back.

What was unmeasured
-------------------
`racer_AI_pathing_inputs()` (game/src/racer.c) is the AI's whole recovery from
"I have driven into something and stopped": after 60 update units of standing
still it kicks the kart into a 60-unit reverse, advances to a different AI node,
and arms a 120-unit cooldown so the recovery cannot re-fire immediately.

    if (racer->unk214 == 0 && racer->velocity < -0.5) {   /* decay */
        racer->unk215 -= updateRate;
        ...
    }
    if (racer->velocity > -1.0 && racer->unk214 == 0 && !gRaceStartTimer && ... &&
        racer->unk215 == 0) {                             /* re-arm gate */
        racer->unk213 += updateRate;
        if (racer->unk213 > 60) { racer->unk214 = 60; racer->unk215 = 120; ... }
    }

The cooldown decays on ONE condition. Note the sign: `racer->velocity` is
NEGATIVE for forward motion (`racer.c` assigns `racer->velocity = -sqrtf(...)`;
the boss animation picks RUN at `velocity < -2.0`), measured at about -11 for an
opponent driving a clean lap and POSITIVE while the AI's own reverse-out state
is running. So `unk214 == 0 && velocity < -0.5` means "the reverse window has
expired AND the kart is back to driving forward at speed" -- not "the kart is
reversing", which is how docs/open-items/gameplay.md read it until this witness
measured it. The conclusion is the same either way, because a kart that cannot
move satisfies neither.

Nothing else in racer.c reads or writes `unk215`. So a kart that comes out of
the reverse window still unable to move has an armed cooldown that can never
reach zero, and a recovery gated on `unk215 == 0` that can therefore never fire
again. It is stuck for the rest of the race.

That is not hypothetical: it is measured, on the AUTOPILOTED HUMAN, at Hot Top
Volcano's crater jump -- 18,677 frames motionless -- and is why
`mdkr_autopilot_unstick()` (platform/mdkr_adventure.c, `MDKR_AUTOPILOT_UNSTICK`)
exists at all. See docs/open-items/gameplay.md.

What was NOT measured is the case that decides how that item is classified: a
GENUINE OPPONENT. The unstick helper is reachable only from the human-autopilot
limb of update_player_racer(), so a real CPU racer has no recovery whatsoever --
if the same wedge is reachable for opponents, every player who races Hot Top
Volcano can watch a rival stand still for two laps, and the item is a
player-visible defect rather than a test-harness inconvenience. It had never
been observed on an opponent, but "never observed" was a statement about where
anyone had looked.

Where the opponents actually are
--------------------------------
Worth stating because it is easy to get wrong from a grep:
update_player_racer()'s `} else { racer_AI_pathing_inputs(...); }` is NOT the
opponent path. It sits inside

    if (tempRacer->playerIndex == PLAYER_COMPUTER) {
        update_AI_racer(obj, tempRacer, updateRate, updateRateF);
    } else {
        ... 200 lines, including that input dispatch ...
    }

so it is reached only by a HUMAN kart that the finish/menu hand-off has already
relabelled PLAYER_COMPUTER. Genuine opponents run `update_AI_racer()`, which
calls `racer_AI_pathing_inputs()` while `racer->unk201 != 0`. The witness this
check reads is installed there, and refuses any racer index ever seen carrying a
real player index.

The measurement
---------------
`MDKR_AI_STUCK_TRACE=<stride>` (game/src/racer.c, inert unless set, deliberately
NOT folded into MDKR_TRACE -- see the "shipping configuration" note below) emits,
for every opponent:

  [AISTUCKEV] ... event=arm    the cooldown went 0 -> 120: a recovery fired
  [AISTUCKEV] ... event=clear  the cooldown reached 0 again, with the peaks
  [AISTUCK]   ... every <stride> ticks, the live state

and maintains two per-opponent counters, both in update-rate units:

  stall    consecutive units with the cooldown armed while |velocity| < 0.5 --
           the exact predicate under which the decay branch cannot be taken
           because the kart is not moving. This is the wedge signature.
  nodecay  consecutive units with the cooldown armed and the decay branch's own
           condition false. Legitimately covers the entire reverse window, so it
           is the loose bound; `stall` is the tight one.

Both reset the moment the kart finishes: a racer parked on the results
choreography is not evidence about the recovery loop.

Where the wedge window comes from
---------------------------------
Measured, not chosen. 32 Hot Top Volcano races across 32 distinct boot RNG seeds
produced 23 legitimate stuck-then-recover episodes on genuine opponents. Every
one of them cleared -- 23 `arm` events, 23 `clear` events, none left armed at the
end of a run. Their peaks:

    stall    2 x8, 3 x8, 4, 6, 7 x3, 15, 229        (23 episodes, max 229)
    nodecay  65 .. 729
    armed    186 .. 1196 update units

The 229 is the interesting one and it is worth reading, because it is what makes
this gate's threshold defensible rather than arbitrary. Opponent 6 left the track
at (-96.1, -987.7, 2357.9), all wheels off a surface, velocity pinned at 0.472 --
below the 0.5 the decay branch needs, in the direction that cannot satisfy it
either -- with the cooldown at its full 120 and the reverse window already
expired. That is the wedge, exactly: 208 update units of a kart that cannot
re-arm its own recovery. It did not stay wedged, because DKR's own
out-of-bounds respawn put it back on the track at tick 14790, after which the
cooldown decayed 120 -> 114 -> 84 -> 54 -> 24 -> 0 in six samples of ordinary
forward driving. The respawn is the opponent's recovery. The human autopilot at
the crater lands INSIDE the world, grounded, so respawn never triggers for it --
which is why the same track wedges the autopilot and not the field.

So the window is set at 4x the observed legitimate maximum: an opponent whose
recovery is genuinely deadlocked has an UNBOUNDED counter (nothing else writes
`unk215`), so anything a real wedge produces will run past this within a second
or two of racing, while the longest legitimate episode ever measured uses a
quarter of it.

Shipping configuration
----------------------
This gate does not export `MDKR_TRACE`. That variable arms engine behaviour as
well as printing (`mdkr_resource_trace_enabled()`; wave "shadowdeep" R1 in
docs/open-items/renderer.md), so a measurement taken with it set is a
measurement of a different program. The witness has its own variable and the
only other thing this check sets is the route.

If this check FAILS
-------------------
Do not "fix" it by changing gameplay. `racer_AI_pathing_inputs()` is matching
decompiled code with no GLOBAL_ASM and no NON_MATCHING; whether the cooldown
should behave this way is an owner decision (preservation port vs. fix), and the
open item exists precisely to hold that decision. Report the telemetry.

    python3 tests/check_ai_unstick_opponents.py --build build-rel -v
    python3 tests/check_ai_unstick_opponents.py --self-test   # reducer control

Always muted + headless, per tests/README.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import (DEFAULT_BUILD_DIR, find_fatal, parse_rows,
                           resolve_binary)

SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"
HOT_TOP_VOLCANO = 7      # the level the only known wedge was measured on
FRAMES = 16000           # the AI field completes three laps by ~15900
STRIDE = 30              # [AISTUCK] sample period, in simulated ticks
RACES = 12               # distinct boot RNG seeds; 32 were measured for the window

# 229 update units is the longest legitimate armed-and-immobile stretch measured
# across 32 races (see the module docstring). A genuine wedge is unbounded, so
# 4x that separates the two without being calibrated on any racing line.
WEDGE_WINDOW = 900
# The loose counter's own measured maximum was 729; same 3-4x headroom. It exists
# to catch a kart that jitters just above the 0.5 threshold without ever
# satisfying the decay branch, which `stall` alone would miss.
NODECAY_WINDOW = 2400

# The opponents in an eight-racer Tracks race. The human is racer 0 and is
# excluded by the witness itself.
MIN_OPPONENTS = 6
MIN_CHECKPOINT = 40      # a race that never got going proves nothing

EVENT_RE = re.compile(
    r"\[AISTUCKEV\] tick=(\d+) racer=(\d+) event=(arm|clear)(.*)")


def seed_for(index: int) -> str:
    """A reproducible spread of boot RNG seeds.

    `MDKR_RNGSEED=0x<hex>` sets `gCurrentRNGSeed` in a constructor
    (platform/math_util_native.c) and nothing re-seeds at boot, so each value is
    a genuinely independent random stream for the whole run: different weather
    rolls, different AI decisions, different contact between karts. Measured on
    this route, seeds diverge the field within one lap -- final checkpoints 89,
    95, 97 and completely different positions at the same tick -- which is what
    makes N races N samples rather than one trajectory repeated.

    Knuth's multiplicative constant against a prime modulus, so the list is a
    fixed, reviewable set rather than something that changes per run.
    """
    return "0x%08x" % ((index * 2654435761) % 4294967291 + 1)


def run_race(binary: str, rom: str, level: int, seed: str, frames: int,
             save_dir: str, verbose: bool) -> tuple[str, int]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",              # belt-and-braces; --headless-frames is the guarantee
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",       # updateRate pinned: the counters are per
                                     # simulated field, not per host frame
        MDKR_AUTOPILOT="1",          # DKR's own AI drives the human, so the race runs
        MDKR_LOAD_TRACK=str(level),
        MDKR_RNGSEED=seed,
        MDKR_AI_STUCK_TRACE=str(STRIDE),
        MDKR_SAVE_DIR=save_dir,
    )
    # Deliberately absent: MDKR_TRACE (see the module docstring) and
    # MDKR_AUTOPILOT_UNSTICK -- this check must observe the game's own recovery,
    # not a test hook standing in for it.
    command = [binary, "--headless-frames", str(frames),
               "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print("  $ MDKR_RNGSEED=%s MDKR_LOAD_TRACK=%d %s"
              % (seed, level, " ".join(command)))
    process = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=900)
    return process.stdout.decode("utf-8", "replace"), process.returncode


def reduce_race(label: str, output: str) -> dict:
    """Everything this check asserts on, from one race's telemetry."""
    rows = parse_rows(output, "AISTUCK")
    events = [(int(m.group(1)), int(m.group(2)), m.group(3), m.group(4))
              for m in EVENT_RE.finditer(output)]

    peaks: dict[int, dict[str, int]] = {}
    for row in rows:
        racer = row.get("racer")
        if racer is None:
            continue
        peak = peaks.setdefault(racer, {"stall": 0, "nodecay": 0, "tick": 0,
                                        "cp": 0, "cooldown": 0})
        for key in ("stall", "nodecay"):
            if row.get(key, 0) > peak[key]:
                peak[key] = row[key]
                if key == "stall":
                    peak["tick"] = row.get("tick", 0)
                    peak["cp"] = row.get("cp", 0)
                    peak["cooldown"] = row.get("cooldown", 0)
    # The episode peaks are exact (maintained per tick in C); the sampled rows
    # can only ever under-report, so the two are combined rather than trusted
    # individually.
    for tick, racer, kind, tail in events:
        if kind != "clear":
            continue
        fields = dict(token.split("=", 1) for token in tail.split()
                      if "=" in token)
        peak = peaks.setdefault(racer, {"stall": 0, "nodecay": 0, "tick": tick,
                                        "cp": 0, "cooldown": 0})
        for key, name in (("stall", "stallPeak"), ("nodecay", "nodecayPeak")):
            value = int(fields.get(name, 0))
            if value > peak[key]:
                peak[key] = value
                if key == "stall":
                    peak["tick"] = tick
                    peak["cp"] = int(fields.get("cp", 0))
    arms = sum(1 for _t, _r, kind, _tail in events if kind == "arm")
    clears = sum(1 for _t, _r, kind, _tail in events if kind == "clear")
    return {
        "label": label,
        "rows": len(rows),
        "opponents": sorted(peaks),
        "peaks": peaks,
        "arms": arms,
        "clears": clears,
        "max_cp": max((row.get("cp", 0) for row in rows), default=0),
        "levels": {row.get("level") for row in rows},
    }


def assert_race(race: dict, level: int, failures: list[str]) -> None:
    label = race["label"]
    if not race["rows"]:
        failures.append(
            "%s: no [AISTUCK] rows -- MDKR_AI_STUCK_TRACE emitted nothing, so "
            "this race observed no opponent at all and every assertion below is "
            "vacuous" % label)
        return
    if len(race["opponents"]) < MIN_OPPONENTS:
        failures.append(
            "%s: telemetry for %d opponents (%s), want >= %d -- an eight-racer "
            "Tracks race lost most of its field, so this is not the population "
            "the check exists to measure"
            % (label, len(race["opponents"]), race["opponents"], MIN_OPPONENTS))
    if race["levels"] - {level}:
        failures.append(
            "%s: opponent telemetry came from level(s) %s, want only %d"
            % (label, sorted(race["levels"]), level))
    if race["max_cp"] < MIN_CHECKPOINT:
        failures.append(
            "%s: the field only reached checkpoint %d (want >= %d) -- the race "
            "never got going, so no opponent was ever in a position to wedge"
            % (label, race["max_cp"], MIN_CHECKPOINT))

    for racer in sorted(race["peaks"]):
        peak = race["peaks"][racer]
        if peak["stall"] > WEDGE_WINDOW:
            failures.append(
                "%s: OPPONENT WEDGE -- racer %d held the AI stuck-recovery "
                "cooldown armed while immobile (|velocity| < 0.5) for %d update "
                "units, past the %d-unit window, peaking around tick %d at "
                "checkpoint %d with cooldown=%d. The cooldown decays only while "
                "`unk214 == 0 && velocity < -0.5` and nothing else in racer.c "
                "writes it, so this racer's recovery cannot re-arm. This is the "
                "case docs/open-items/gameplay.md records as never observed on a "
                "genuine opponent. DO NOT change gameplay to make this pass: "
                "report the telemetry -- whether the port keeps DKR's behaviour "
                "or fixes it is an owner decision"
                % (label, racer, peak["stall"], WEDGE_WINDOW, peak["tick"],
                   peak["cp"], peak["cooldown"]))
        elif peak["nodecay"] > NODECAY_WINDOW:
            failures.append(
                "%s: OPPONENT WEDGE (jittering) -- racer %d held the cooldown "
                "armed for %d update units without once satisfying its decay "
                "condition, past the %d-unit window, around tick %d at "
                "checkpoint %d. Same deadlock as above with the kart vibrating "
                "instead of standing still. Report it; do not change gameplay"
                % (label, racer, peak["nodecay"], NODECAY_WINDOW, peak["tick"],
                   peak["cp"]))


def self_test() -> int:
    """Prove the reducer and the assertion can see a wedge.

    A wedge on a genuine opponent has never been produced on demand -- the only
    known repro wedges the autopiloted human, which this check deliberately does
    not observe -- so the positive control is synthetic telemetry in exactly the
    shape the engine emits. It proves the reader, the counters' interpretation
    and the threshold; the engine-side witness is proven live by the 23 real
    cooldown episodes the passing run reports.
    """
    wedged = "\n".join(
        "[AISTUCK] tick=%d level=7 racer=3 player=-1 cooldown=120 reverse=0 "
        "idle=0 vel=0.472 grounded=0 cp=41 lap=1 finished=0 stall=%d "
        "nodecay=%d x=-96.1 y=-987.7 z=2357.9"
        % (5000 + step * STRIDE, step * STRIDE, step * STRIDE)
        for step in range(1, 60))
    healthy = "\n".join(
        "[AISTUCK] tick=%d level=7 racer=%d player=-1 cooldown=0 reverse=0 "
        "idle=0 vel=-11.068 grounded=4 cp=41 lap=1 finished=0 stall=0 "
        "nodecay=0 x=1.0 y=2.0 z=3.0" % (5000 + step * STRIDE, racer)
        for step in range(1, 60) for racer in range(1, 8))
    good_events = ("[AISTUCKEV] tick=4900 racer=5 event=arm cooldown=120 "
                   "reverse=60 idle=0 cp=11\n"
                   "[AISTUCKEV] tick=5100 racer=5 event=clear armed=200 "
                   "stallPeak=229 nodecayPeak=729 cp=12")

    problems: list[str] = []
    healthy_race = reduce_race("self-test/healthy", healthy + "\n" + good_events)
    healthy_failures: list[str] = []
    assert_race(healthy_race, HOT_TOP_VOLCANO, healthy_failures)
    if healthy_failures:
        problems.append("the healthy control failed: %s" % healthy_failures)
    if healthy_race["arms"] != 1 or healthy_race["clears"] != 1:
        problems.append("the healthy control lost its cooldown episode")
    if healthy_race["peaks"][5]["stall"] != 229:
        problems.append("the reducer did not read the episode's exact peak")

    wedge_failures: list[str] = []
    assert_race(reduce_race("self-test/wedged", wedged + "\n" + healthy),
                HOT_TOP_VOLCANO, wedge_failures)
    if not any("OPPONENT WEDGE" in message for message in wedge_failures):
        problems.append("a %d-unit stall was NOT reported as a wedge: %s"
                        % (59 * STRIDE, wedge_failures))

    if problems:
        for problem in problems:
            print("  - %s" % problem, file=sys.stderr)
        print("check_ai_unstick_opponents: SELF-TEST FAIL")
        return 1
    print("check_ai_unstick_opponents: SELF-TEST PASS  (a %d-unit opponent "
          "stall is reported; the measured %d-unit legitimate episode is not)"
          % (59 * STRIDE, 229))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--level", type=int, default=HOT_TOP_VOLCANO)
    parser.add_argument("--races", type=int, default=RACES)
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--self-test", action="store_true",
                        help="run the reducer's positive control and exit")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    races: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="mdkr_ai_unstick_") as root:
        for index in range(args.races):
            seed = seed_for(index)
            label = "race %d (seed %s)" % (index, seed)
            # A save dir per race. These races finish, and a recorded time
            # changes the frontend route the next run navigates -- which would
            # make race N's field depend on race N-1 and quietly turn N samples
            # back into one trajectory.
            save_dir = os.path.join(root, "save-%d" % index)
            os.makedirs(save_dir)
            output, code = run_race(binary, args.rom, args.level, seed,
                                    args.frames, save_dir, args.verbose)
            if code != 0:
                failures.append("%s: exit code %d" % (label, code))
            fatal = find_fatal(output)
            if fatal is not None:
                failures.append("%s: %s" % (label, fatal))
            race = reduce_race(label, output)
            races.append(race)
            assert_race(race, args.level, failures)
            if args.verbose:
                print("  %s: %d rows, opponents %s, %d arm / %d clear, "
                      "peak stall %d, peak nodecay %d, max cp %d"
                      % (label, race["rows"], race["opponents"], race["arms"],
                         race["clears"],
                         max((p["stall"] for p in race["peaks"].values()),
                             default=0),
                         max((p["nodecay"] for p in race["peaks"].values()),
                             default=0),
                         race["max_cp"]))

    arms = sum(race["arms"] for race in races)
    clears = sum(race["clears"] for race in races)
    stall = max((peak["stall"] for race in races
                 for peak in race["peaks"].values()), default=0)
    nodecay = max((peak["nodecay"] for race in races
                   for peak in race["peaks"].values()), default=0)

    # Fail closed. If no opponent ever armed the cooldown, this check watched
    # the right racers and learned nothing about the mechanism -- which is
    # indistinguishable from a witness that stopped reporting.
    if arms == 0:
        failures.append(
            "no opponent armed the stuck-recovery cooldown in %d races. The "
            "gate is therefore not exercising the mechanism it exists to bound: "
            "either the witness stopped reporting, or the route/level changed "
            "enough that opponents no longer contact anything. 32 measured "
            "races produced 23 episodes, so zero in %d is not a plausible sample"
            % (args.races, args.races))

    if failures:
        for message in failures:
            print("  - %s" % message, file=sys.stderr)
        print("check_ai_unstick_opponents: FAIL")
        return 1
    print("check_ai_unstick_opponents: PASS  (%d races on level %d, %d opponent "
          "cooldown episodes, %d cleared; longest armed-and-immobile stretch %d "
          "update units, longest armed-without-decay %d, windows %d / %d)"
          % (args.races, args.level, arms, clears, stall, nodecay,
             WEDGE_WINDOW, NODECAY_WINDOW))
    return 0


if __name__ == "__main__":
    sys.exit(main())
