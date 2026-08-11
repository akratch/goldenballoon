#!/usr/bin/env python3
"""Opponent skill: `authored` is the authored game, `hard` and `brutal` are not.

What this gate is for
---------------------
`Enhancements.AIDifficulty` is the port's only MDKR_ENH_GAMEPLAY enhancement
(platform/enhancement_registry.c). Being allowed to change the simulation is
exactly what makes it dangerous, so it needs two separate proofs, and they pull
in opposite directions:

  PURITY   at `authored` the enhancement must be invisible. Not "close", not
           "equivalent" -- the authoritative [SIMHASH] v3 stream must be
           byte-identical to a build in which the enhancement's call site is
           NOT COMPILED IN AT ALL. This gate builds that binary itself (see
           `build_baseline`), so the claim is measured against a real absence
           rather than against the same code with a 1.0 multiplier in it.

  EFFECT   at `hard` and `brutal` the opponents must genuinely race better,
           measured as the MEAN OPPONENT FINISH POSITION over seeded races, and
           they must do it without breaking anything: no wedged opponent, and no
           lap time that stops being physically plausible.

How "better" is measured, and why that measurement is not circular
------------------------------------------------------------------
`finishPosition` (game/src/objects.c) is DKR's own number, read out of the
per-racer `[ORACLE]` telemetry (MDKR_ORACLE_STATE=1, game/src/objects.c) at the
last tick of the run. In an eight-kart field the positions are a permutation of
1..8, so the mean over the seven opponents is fully determined by where the
PLAYER's kart lands: it is (36 - player position) / 7. Lower is better.

That is only a usable measurement because of a measured fact about this route:
on Ancient Lake, DKR's own AI driving the player's kart (MDKR_AUTOPILOT=1) WINS
at the authored setting on every seed tried -- so the authored arm sits at the
metric's ceiling of 5.0 and every kart the enhancement pushes past the player
moves it down. The route matters: the same fixture on Hot Top Volcano finishes
the autopiloted player LAST at the authored setting, which pins the mean at 4.0
and would make this assertion unfalsifiable. If this check is ever pointed at
another track, re-measure the authored arm first and check it is not already on
the floor.

What it asserts
---------------
  1. Every arm's races complete: eight karts, eight distinct finish positions.
     A race that did not finish is not evidence about finishing positions.
  2. `authored` on this build produces a [SIMHASH] v3 stream byte-identical to
     `authored` on the compiled-out baseline build, per seed.
  3. `authored` never applies the scale -- the binary's own `[AIDIFF]` witness
     must report the arm resolved and must NOT report an application.
  4. `hard` and `brutal` each finish with a strictly better (lower) mean
     opponent finish position than `authored`.
  5. No arm wedges an opponent. The assertion is IMPORTED from
     tests/check_ai_unstick_opponents.py -- `reduce_race` and `assert_race`,
     with that module's own measured windows -- rather than restated here, so
     there is exactly one definition of "wedged" in the repository.
  6. Lap times stay physically plausible. The floor is derived, not chosen: the
     enhancement multiplies opponent top speed by at most the scale the binary
     itself reports for `brutal`, so nothing can lap faster than the authored
     best lap divided by that scale. Anything below that (with a margin) is a
     lap the game counted without the kart driving it.

The positive control
--------------------
`--positive-control` re-runs assertion 4 against the compiled-out baseline
binary, which is exactly "force the scale to 1.0 in every arm". There the three
arms are the same race, so `hard` cannot beat `authored` and the assertion MUST
fail. The control passes when that failure happens and fails when it does not.

Usage:
    tests/check_enh_ai_difficulty.py [--build build] [--rom baserom.us.v80.z64]
                                     [--races N] [--frames N] [--level N]
                                     [--positive-control] [-v]

Always muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary
# The recovery assertion, and the seed spread, come from the gate that owns
# them. Importing rather than restating is deliberate: a second definition of
# "wedged" would be a second thing to keep in step with racer.c.
from check_ai_unstick_opponents import (STRIDE, assert_race, reduce_race,
                                        seed_for)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"

# Ancient Lake. See the module docstring for why the track is part of the
# measurement and not an incidental choice.
ANCIENT_LAKE = 5
# The whole eight-kart field is home by tick ~8630 on this route; the slack is
# for an arm that reshuffles the order without making anyone quicker.
FRAMES = 11000
# The purity arm only has to reach racing, not finish. The lights go out around
# tick 3115 here, so this is roughly 2900 ticks of scaled opponents.
PURITY_FRAMES = 6000
PURITY_RACES = 2
RACES = 6
HASH_VERSION = "3"

ARMS = ("authored", "hard", "brutal")

# How much slower than "the authored best lap, driven at the largest scale this
# build applies" a lap is still allowed to be reported as. The 10% is slack for
# a different racing line and a different set of item hits, not for a different
# speed: the point of the floor is to catch a lap the kart never drove.
LAP_FLOOR_MARGIN = 0.90

# The compiled-out baseline. `MDKR_ENH_AI_DIFFICULTY_OMIT` removes the call in
# handle_racer_top_speed() (game/src/racer.c) from the translation unit
# entirely, so the baseline binary's racer.c is the file as it was before this
# enhancement existed. The module itself is still linked and simply never
# called, which is why the docstring says "call site" rather than "code".
OMIT_DEFINE = "-DMDKR_ENH_AI_DIFFICULTY_OMIT"
BASELINE_SUFFIX = "-aidiff-omit"

ORACLE_RE = re.compile(
    r"\[ORACLE\] frame=(?P<frame>\d+) map=(?P<map>-?\d+) slot=(?P<slot>\d+) .*?"
    r" cp=(?P<cp>-?\d+) next=-?\d+ lap=(?P<lap>-?\d+) "
    r"countlap=(?P<countlap>-?\d+) fin=(?P<fin>-?\d+) fpos=(?P<fpos>-?\d+) "
    r"ridx=(?P<ridx>-?\d+) pidx=(?P<pidx>-?\d+) .*? clock=(?P<clock>\d+)")
AIDIFF_RESOLVE_RE = re.compile(
    r"\[AIDIFF\] event=resolve arm=(\S+) scale=([\d.]+)")
AIDIFF_APPLY_RE = re.compile(r"\[AIDIFF\] event=apply arm=(\S+)")


# --------------------------------------------------------------------------
#  Running the game
# --------------------------------------------------------------------------

def base_env(save_dir: Path) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",              # belt and braces; --headless-frames is the guarantee
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",       # updateRate pinned, so one tick is one frame
        MDKR_AUTOPILOT="1",          # DKR's own AI drives the player, so the race runs
        MDKR_AIDIFF_TRACE="1",       # [AIDIFF] arm/scale witness
        MDKR_SAVE_DIR=str(save_dir),
        # The scripted menu route is authored against default input and video
        # settings. A maintainer's real launcher preferences must not decide
        # whether this release gate reaches the race it claims to measure.
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    # Deliberately absent: MDKR_TRACE. It arms engine behaviour as well as
    # printing (see tests/check_ai_unstick_opponents.py), so a run with it set
    # is a measurement of a different program.
    return env


def run_race(binary: Path, rom: Path, level: int, seed: str, frames: int,
             save_dir: Path, arm: str, *, state_hash: bool,
             verbose: bool) -> str:
    """One headless race. Returns its combined output."""
    save_dir.mkdir(parents=True, exist_ok=True)
    env = base_env(save_dir)
    env["MDKR_LOAD_TRACK"] = str(level)
    env["MDKR_RNGSEED"] = seed
    if state_hash:
        env["MDKR_STATE_HASH"] = HASH_VERSION
    else:
        env["MDKR_ORACLE_STATE"] = "1"       # per-racer finish positions
        env["MDKR_AI_STUCK_TRACE"] = str(STRIDE)  # the imported recovery witness
    command = [str(binary), "--headless-frames", str(frames),
               "--input-script", SCRIPT, "--rom", str(rom),
               "--video-set", f"Enhancements.AIDifficulty={arm}"]
    if verbose:
        print("  $ MDKR_RNGSEED=%s %s" % (seed, " ".join(command)), flush=True)
    proc = subprocess.run(command, cwd=str(ROOT), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=1800, check=False)
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise RuntimeError("exit %d\n%s" % (proc.returncode, output[-3000:]))
    return output


# --------------------------------------------------------------------------
#  The compiled-out baseline build
# --------------------------------------------------------------------------

def mirrored_cache_args(build: Path) -> list[str]:
    """Reproduce the build under test's configuration for the baseline.

    Only the entries that decide what code is compiled are carried across. A
    baseline configured differently from the build it is compared with would
    turn any configuration difference into a purity failure, which is the one
    way this gate could report a bug that is not there.
    """
    args: list[str] = []
    cache = build / "CMakeCache.txt"
    wanted = ("CMAKE_BUILD_TYPE", "MDKR_APP", "MDKR_WEBGPU_BACKEND",
              "MDKR_VERSION", "MDKR_BUILD_FUZZERS", "CMAKE_OSX_ARCHITECTURES")
    extra = ""
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            if ":" not in line or "=" not in line:
                continue
            name = line.split(":", 1)[0]
            value = line.split("=", 1)[1]
            if name in wanted:
                args.append(f"-D{name}={value}")
            elif name == "MDKR_EXTRA_C_FLAGS":
                extra = value
    args.append("-DMDKR_EXTRA_C_FLAGS=" +
                (extra + " " + OMIT_DEFINE if extra else OMIT_DEFINE))
    return args


def build_baseline(build: Path, verbose: bool) -> Path:
    """Configure and build the binary this enhancement is not compiled into."""
    baseline = build.parent / (build.name + BASELINE_SUFFIX)
    jobs = str(os.cpu_count() or 4)
    steps = (
        ["cmake", "-S", str(ROOT), "-B", str(baseline)] +
        mirrored_cache_args(build),
        ["cmake", "--build", str(baseline), "-j" + jobs, "--target", "mdkr64"],
    )
    for step in steps:
        if verbose:
            print("  $ " + " ".join(step), flush=True)
        proc = subprocess.run(step, cwd=str(ROOT), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=3600,
                              check=False)
        if proc.returncode != 0:
            raise RuntimeError(
                "the purity baseline could not be built, so the strongest "
                "assertion in this gate cannot run:\n%s"
                % proc.stdout.decode("utf-8", "replace")[-4000:])
    return baseline / "mdkr64"


# --------------------------------------------------------------------------
#  Reducing one race
# --------------------------------------------------------------------------

class Race:
    """Everything this gate asserts on, from one race's telemetry."""

    def __init__(self, label: str, arm: str, output: str):
        self.label = label
        self.arm = arm
        self.problems: list[str] = []

        final: dict[int, dict[str, int]] = {}
        finish_frame: dict[int, int] = {}
        finish_countlap: dict[int, int] = {}
        humans: set[int] = set()
        lap_clock: dict[int, int] = {}
        rows = 0

        for line in output.splitlines():
            match = ORACLE_RE.search(line)
            if match is None:
                continue
            rows += 1
            row = {name: int(value) for name, value in match.groupdict().items()}
            slot = row["slot"]
            final[slot] = row
            # A racer that ever carried a real player index is the human, for
            # the rest of the run: update_player_racer() relabels a FINISHED
            # human kart PLAYER_COMPUTER, so the last row cannot tell them
            # apart. Same latch the AISTUCK witness uses in racer.c.
            if row["pidx"] >= 0:
                humans.add(slot)
                if row["countlap"] not in lap_clock:
                    lap_clock[row["countlap"]] = row["clock"]
            if row["fin"] and slot not in finish_frame:
                finish_frame[slot] = row["frame"]
                finish_countlap[slot] = row["countlap"]

        self.rows = rows
        self.final = final
        self.humans = humans
        self.finish_frame = finish_frame
        self.lap_clock = lap_clock

        resolve = AIDIFF_RESOLVE_RE.search(output)
        self.resolved_arm = resolve.group(1) if resolve else None
        self.resolved_scale = float(resolve.group(2)) if resolve else None
        self.applied = AIDIFF_APPLY_RE.search(output) is not None

        self.mean_opponent_position: float | None = None
        self.human_position = 0
        self.laps = 0
        self.lap_times: list[int] = []
        self.course_ticks: dict[int, int] = {}
        self._derive(finish_countlap)

    def _derive(self, finish_countlap: dict[int, int]) -> None:
        if not self.rows:
            self.problems.append(
                "no [ORACLE] rows -- MDKR_ORACLE_STATE emitted nothing, so "
                "every assertion below would be vacuous")
            return
        if len(self.humans) != 1:
            self.problems.append(
                "expected exactly one human kart, saw %s" % sorted(self.humans))
            return
        human = next(iter(self.humans))

        positions = {slot: row["fpos"] for slot, row in self.final.items()}
        unplaced = sorted(s for s, p in positions.items() if p <= 0)
        if unplaced:
            self.problems.append(
                "kart(s) %s never got a finish position in %d ticks -- the "
                "race did not finish, so its finishing order is not evidence"
                % (unplaced, max(row["frame"] for row in self.final.values())))
            return
        if sorted(positions.values()) != list(range(1, len(positions) + 1)):
            self.problems.append(
                "finish positions %s are not a permutation of 1..%d"
                % (sorted(positions.values()), len(positions)))
            return

        opponents = [p for slot, p in positions.items() if slot != human]
        self.mean_opponent_position = sum(opponents) / len(opponents)
        self.human_position = positions[human]

        # The race clock is the player's, and under MDKR_SYNTH_FIELDS=1 it
        # advances one unit per frame, so the tick the lights went out on is the
        # player's finish frame minus its frozen clock. Every kart's course time
        # is measured from there.
        self.laps = finish_countlap.get(human, 0)
        start = self.finish_frame[human] - self.final[human]["clock"]
        self.course_ticks = {slot: frame - start
                             for slot, frame in self.finish_frame.items()}

        previous = 0
        for lap in range(1, self.laps + 1):
            if lap not in self.lap_clock:
                continue
            self.lap_times.append(self.lap_clock[lap] - previous)
            previous = self.lap_clock[lap]
        if not self.lap_times:
            self.problems.append(
                "the player's lap boundaries were never observed, so no lap "
                "time could be derived")


# --------------------------------------------------------------------------
#  Assertions
# --------------------------------------------------------------------------

def check_arm_witness(race: Race, arm: str, failures: list[str]) -> None:
    if race.resolved_arm is None:
        failures.append(
            "%s: the binary printed no [AIDIFF] resolve line, so it never read "
            "Enhancements.AIDifficulty and this race is not the arm it is "
            "labelled with" % race.label)
        return
    if race.resolved_arm != arm:
        failures.append(
            "%s: asked for arm '%s', the binary resolved '%s'. Either "
            "--video-set stopped reaching this key or the arm names moved"
            % (race.label, arm, race.resolved_arm))
    if arm == "authored":
        if race.resolved_scale != 1.0:
            failures.append(
                "%s: the authored arm resolved to scale %s, not exactly 1.0"
                % (race.label, race.resolved_scale))
        if race.applied:
            failures.append(
                "%s: the authored arm APPLIED the scale to a racer. The "
                "authored path must return before any arithmetic happens"
                % race.label)
    elif not race.applied:
        failures.append(
            "%s: arm '%s' resolved but never reached a racer -- the call site "
            "is not being taken, so the arm is inert" % (race.label, arm))


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


def compare_arms(results: dict[str, list[Race]],
                 failures: list[str]) -> dict[str, float]:
    """Assertion 4. Returns the per-arm mean, for the caller to print."""
    means = {arm: mean([r.mean_opponent_position for r in races])
             for arm, races in results.items()}
    authored = means["authored"]
    for arm in ("hard", "brutal"):
        if not means[arm] < authored:
            failures.append(
                "%s did not make the opponents race better: mean opponent "
                "finish position %.4f, authored %.4f (lower is better). "
                "Per race, %s: %s vs authored: %s"
                % (arm, means[arm], authored, arm,
                   [r.mean_opponent_position for r in results[arm]],
                   [r.mean_opponent_position for r in results["authored"]]))
    return means


def check_lap_plausibility(results: dict[str, list[Race]],
                           max_scale: float, failures: list[str]) -> float:
    """Assertion 6. Returns the floor it used."""
    authored_best = min(min(r.lap_times) for r in results["authored"])
    floor = authored_best / max_scale * LAP_FLOOR_MARGIN
    for arm, races in results.items():
        for race in races:
            fastest_lap = min(race.lap_times)
            if fastest_lap < floor:
                failures.append(
                    "%s: a %d-tick lap is below the %.0f-tick floor. The "
                    "authored best lap is %d ticks and this build scales "
                    "opponent top speed by at most %.2f, so nothing can lap "
                    "faster than %.0f ticks by driving"
                    % (race.label, fastest_lap, floor, authored_best,
                       max_scale, authored_best / max_scale))
            for slot, ticks in race.course_ticks.items():
                per_lap = ticks / race.laps
                if per_lap < floor:
                    failures.append(
                        "%s: kart %d averaged %.0f ticks a lap over %d laps, "
                        "below the %.0f-tick floor derived from the authored "
                        "best lap of %d and the %.2f cap"
                        % (race.label, slot, per_lap, race.laps, floor,
                           authored_best, max_scale))
    return floor


# --------------------------------------------------------------------------
#  The three phases
# --------------------------------------------------------------------------

def measure_arms(binary: Path, rom: Path, work: Path, level: int, races: int,
                 frames: int, tag: str, failures: list[str],
                 verbose: bool) -> dict[str, list[Race]]:
    results: dict[str, list[Race]] = {}
    for arm in ARMS:
        results[arm] = []
        print("  %s: %d race(s) on arm '%s'" % (tag, races, arm), flush=True)
        for index in range(races):
            seed = seed_for(index)
            label = "%s/%s race %d (seed %s)" % (tag, arm, index, seed)
            # A save directory per race: these races finish, and a recorded
            # time changes the route the next run navigates, which would make
            # race N depend on race N-1.
            save_dir = work / tag / arm / ("save-%d" % index)
            try:
                output = run_race(binary, rom, level, seed, frames, save_dir,
                                  arm, state_hash=False, verbose=verbose)
            except RuntimeError as error:
                failures.append("%s: %s" % (label, error))
                continue
            fatal = find_fatal(output)
            if fatal is not None:
                failures.append("%s: %s" % (label, fatal))

            race = Race(label, arm, output)
            check_arm_witness(race, arm, failures)
            failures.extend("%s: %s" % (label, p) for p in race.problems)

            # Assertion 5, imported whole. Only the AISTUCK lines are handed
            # over: reduce_race reads nothing else, and the [ORACLE] stream is
            # tens of megabytes.
            stuck = "\n".join(line for line in output.splitlines()
                              if "[AISTUCK" in line)
            assert_race(reduce_race(label, stuck), level, failures)

            if race.mean_opponent_position is not None and race.lap_times:
                results[arm].append(race)
                if verbose:
                    print("  %s: player %d, mean opponent %.4f, laps %s"
                          % (label, race.human_position,
                             race.mean_opponent_position, race.lap_times),
                          flush=True)
    for arm in ARMS:
        if len(results[arm]) != races:
            failures.append(
                "arm '%s' produced %d usable races of %d; a partial sample is "
                "not a mean" % (arm, len(results[arm]), races))
    return results


def check_purity(binary: Path, baseline: Path, rom: Path, work: Path,
                 level: int, failures: list[str], verbose: bool) -> int:
    """Assertion 2. Returns the number of [SIMHASH] rows compared."""
    compared = 0
    for index in range(PURITY_RACES):
        seed = seed_for(index)
        streams = {}
        for name, exe in (("with", binary), ("without", baseline)):
            label = "purity/%s race %d (seed %s)" % (name, index, seed)
            try:
                output = run_race(exe, rom, level, seed, PURITY_FRAMES,
                                  work / "purity" / name / ("save-%d" % index),
                                  "authored", state_hash=True, verbose=verbose)
            except RuntimeError as error:
                failures.append("%s: %s" % (label, error))
                return compared
            rows = [line for line in output.splitlines()
                    if line.startswith("[SIMHASH]")]
            if not rows:
                failures.append(
                    "%s: no [SIMHASH] rows; the instrument did not arm, so a "
                    "match below would mean nothing" % label)
                return compared
            streams[name] = rows
        if streams["with"] != streams["without"]:
            first = next(i for i, (a, b) in
                         enumerate(zip(streams["with"], streams["without"]))
                         if a != b)
            failures.append(
                "seed %s: the authored arm is NOT the authored game. The "
                "[SIMHASH] v3 stream differs from the build with the call site "
                "compiled out, first at row %d:\n      with:    %s\n"
                "      without: %s"
                % (seed, first, streams["with"][first],
                   streams["without"][first]))
        compared += len(streams["with"])
    return compared


def positive_control(baseline: Path, rom: Path, work: Path, level: int,
                     races: int, frames: int, verbose: bool) -> bool:
    """Assertion 4 must FAIL when the scale is 1.0 in every arm.

    The compiled-out baseline is that condition exactly, and it has the
    property a hand-edited constant does not: there is nothing to restore
    afterwards, so the control cannot be left switched on by accident.
    """
    # measure_arms' own witness checks fire here BY DESIGN -- on this binary
    # `hard` resolves and never reaches a racer -- so its failure list is
    # collected and then discarded. Only whether the races completed is read
    # from this run; the assertion under test is re-evaluated below.
    setup_failures: list[str] = []
    results = measure_arms(baseline, rom, work, level, races, frames,
                           "control", setup_failures, verbose)
    if any(len(results[arm]) != races for arm in ARMS):
        print("  positive control INCONCLUSIVE: the baseline races did not all "
              "complete (%d/%d/%d usable)"
              % tuple(len(results[arm]) for arm in ARMS))
        return False
    control_failures: list[str] = []
    means = compare_arms(results, control_failures)
    print("  positive control: with the enhancement compiled out the arms "
          "measure authored=%.4f hard=%.4f brutal=%.4f"
          % (means["authored"], means["hard"], means["brutal"]))
    if not control_failures:
        print("  positive control FAILED: 'hard beats authored' still passed "
              "with the scale removed, so that assertion does not depend on "
              "the enhancement and proves nothing")
        return False
    print("  positive control ok: 'hard beats authored' fails without the "
          "enhancement -- %s" % control_failures[0].split(". Per race")[0])
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--level", type=int, default=ANCIENT_LAKE)
    parser.add_argument("--races", type=int, default=RACES)
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--positive-control", action="store_true",
                        help="also prove assertion 4 fails without the effect")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(Path(args.build).expanduser())).resolve()
    build = binary.parent
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, ROOT / SCRIPT):
        if not path.exists():
            print("check_enh_ai_difficulty: FAIL -- missing %s" % path)
            return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr_enh_ai_") as tmp:
        work = Path(tmp)

        print("  building the compiled-out purity baseline "
              "(%s)" % OMIT_DEFINE, flush=True)
        try:
            baseline = build_baseline(build, args.verbose)
        except RuntimeError as error:
            print("check_enh_ai_difficulty: FAIL -- %s" % error)
            return 1

        rows = check_purity(binary, baseline, rom, work, args.level, failures,
                            args.verbose)
        print("  purity: %d [SIMHASH] v3 rows compared across %d seed(s) "
              "against %s" % (rows, PURITY_RACES, baseline), flush=True)

        results = measure_arms(binary, rom, work, args.level, args.races,
                               args.frames, "arms", failures, args.verbose)

        means = None
        floor = None
        if all(len(results[arm]) == args.races for arm in ARMS):
            means = compare_arms(results, failures)
            max_scale = max(r.resolved_scale or 1.0
                            for races in results.values() for r in races)
            floor = check_lap_plausibility(results, max_scale, failures)

        control_ok = True
        if args.positive_control:
            control_ok = positive_control(baseline, rom, work, args.level,
                                          args.races, args.frames,
                                          args.verbose)

    if means is not None:
        for arm in ARMS:
            scale = results[arm][0].resolved_scale
            print("  %-8s scale %.2f  mean opponent finish position %.4f  "
                  "player finished %s"
                  % (arm, scale, means[arm],
                     [r.human_position for r in results[arm]]))
    if floor is not None:
        print("  lap floor %.0f ticks; fastest lap seen %d ticks"
              % (floor, min(min(r.lap_times) for races in results.values()
                            for r in races)))

    if failures or not control_ok:
        print("check_enh_ai_difficulty: FAIL")
        for message in failures:
            print("  - %s" % message)
        return 1
    print("check_enh_ai_difficulty: PASS  (%d races per arm on level %d; "
          "authored is byte-identical to the build without the enhancement; "
          "hard and brutal both race better without wedging an opponent)"
          % (args.races, args.level))
    return 0


if __name__ == "__main__":
    sys.exit(main())
