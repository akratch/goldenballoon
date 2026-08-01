#!/usr/bin/env python3
"""Presentation-rate invariance across CONTENT, not across one route (spec 12.3).

Why this exists, and what it is not
-----------------------------------

``check_presentation_matrix.py`` proves that raising the presentation rate does
not move the authoritative tick -- on ONE level, Ancient Lake, in a car, one
player, NTSC. Spec 12.3 ("Oracle breadth before default promotion") says that
lane is "necessary evidence but not sufficient breadth by itself", and lists
what sufficient looks like: every boss, representative car/hovercraft/plane
races, standard AI races, battle and all challenge types, 1P through 4P, hub and
progression transitions, cutscenes, NTSC and PAL, and a long-running soak.

This gate is that breadth, mechanised for the explicitly test-only replay
implementation. Production 1.0.1 keeps delayed replay disabled; its fail-closed
surface behavior is pinned separately by the matrix and arbitrary-rate gates.
This suite re-uses the matrix gate's authority comparison --
comparison -- the per-tick ``[SIMHASH]`` stream with ``MDKR_PRESENT_RATE``
unset versus engaged must be BYTE-IDENTICAL -- and runs it over a content set
instead of a single route, via the ``MDKR_LOAD_TRACK=<level>[:<vehicle>]`` hook
that retargets the fixture route's race load.

It also records, per arm, presentation-side quantities that can degrade
without moving a single authoritative bit, and therefore cannot be caught by the
hash comparison at all:

  * the replay walk's matrix RECOMPOSITION REJECT RATIO -- gameplay matrices
    whose (world, view_projection) decomposition did not reproduce the display
    list, which therefore keep the TICK's camera while the rest of the scene
    gets the interpolated one;
  * the RECOMPOSITION TOLERANCE's two populations and the gap between them:
    how many mismatches were accepted as float re-association, how far the
    worst accepted one was, and how far the NEAREST rejected one was. The
    threshold is only principled while those two are orders of magnitude apart,
    so the gap is re-measured every run rather than trusted from the doc;
  * matrix registry FREEZE/RESTORE failures, which silently drop a frame of
    registrations;
  * snapshot capture OVERFLOWS, which drop a whole publish and leave the
    previous pair in place;
  * retained-packet FREEZE FAILURES and ambiguous deformation keys, which
    safely hold animation but expose a capacity or stable-recipe gap;
  * retained point/line particle batches and phase-correct exact endpoint
    holds. The battle-challenge arm is the positive capture witness; until a
    task-to-next endpoint packet exists, any changed XYZ/RGBA blend is a lagged
    gameplay regression and must remain zero;
  * INTERPOLATION CONTINUITY -- how many presents found nothing to draw
    (``stale``) and how many actually substituted a camera.

The full 0-65 sweep behind this gate's subset
---------------------------------------------

Levels 0-65 minus 37/41/54 -- 63 levels, every one reaching its race -- were run
once at 5,000 ticks per arm on this branch, and all 63 were byte-identical with
zero overflows and zero freeze/restore failures. Levels 37/41/54 are excluded
because they are the subject of a separate, unrelated defect hunt; measured
here, 37 and 41 are self-consistent (a base-vs-base rerun is byte-identical) and
54 is NOT -- it diverges from itself at tick 2621, its own race load -- so
excluding all three is conservative rather than convenient.

This gate runs a SUBSET of that sweep, chosen for spec 12.3's shape rather than
for coverage arithmetic, because 63 levels x 2 arms is ~45 minutes and the suite
is sequential. Each entry states which 12.3 clause it stands for. The subset
deliberately includes both extremes of the historical pre-tolerance reject
ratio (level 17 at 22.2%, level 21 at 0.012%). The verified geometric tolerance
now accepts the benign re-association population and all 17 current arms report
zero hard rejects; retaining those endpoints makes a tolerance regression
visible against the broadest known numeric spread.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
SCRIPT_4P = ROOT / "tests" / "input_scripts" / "race_4p_split.txt"

# The fixture route reaches its retargeted race load at tick 2621, so a budget
# below that measures menus only -- an early version of this sweep ran 1,200
# ticks and every level produced BYTE-IDENTICAL telemetry, because none of them
# had loaded yet. 4,200 leaves ~1,580 in-race ticks per arm.
TICKS = 4200
RACE_LOAD_TICK = 2621
# 4P joins four controllers through the character/vehicle prompts first.
TICKS_4P = 4800

# Every content arm uses the same complete authority contract as the focused
# matrix and render-purity gates.
HASH_VERSION = "3"

# Presents per tick for this breadth gate's selected 60 Hz NTSC / 50 Hz PAL
# arms. Arbitrary and sub-field rates, including PAL 60, are covered separately
# by check_arbitrary_presentation_rates.py.
PRESENTS_PER_TICK = 2

# Bounds, all set from the measured 63-level sweep (see the doc above), and all
# far from the measured worst rather than fitted to it.
#
# MAX_REJECT_RATIO is 0.50 -- deliberately the SAME bound
# check_render_purity.py's arm E uses, not a looser one. The current hard-reject
# population is zero; 22.2% on level 17 was the historical pre-tolerance worst.
# A recomposition regressed to near-total rejection still passes every hash and
# pixel assertion because fallback is bit-exact, so the ratio plus the tenancy
# positive control remain necessary.
MAX_REJECT_RATIO = 0.50
# stale = presents that drew nothing. Measured 11-13 per 5,000 ticks (0.26%) on
# every one of the 63 levels; they are the stage-transition passes where the
# game submitted no graphics task. 1% is two orders of margin over the spread
# and still catches a subloop that has stopped drawing.
MAX_STALE_RATIO = 0.01

# ---- the recomposition tolerance (spec 12.4) --------------------------------
#
# The reject ratio above used to be 5-22% on race content, and 99.96% of it was
# a correct decomposition that re-associated a float product. Each such reject
# drew its geometry with the TICK's camera on an interpolated frame, so a frame
# mixed two cameras across up to a fifth of its scene. The verification is now a
# distance rather than an identity on the vp-overridden path
# (DKR_RECOMPOSE_TOLERANCE_LSB = 4096 s15.16 LSBs = 1/16 world unit,
# gfx_pc_dkr.c). The stale-tenant guard and owner-specific replay now leave no
# production hard-reject population at all.
#
# These four bounds are the tolerance's gate, and they are deliberately shaped to
# fail from BOTH sides -- a tolerance that stopped working and a tolerance that
# grew until it swallowed a real defect are different failures, and one bound
# cannot see both.
#
# HARD REJECTS ARE NOW BOUNDED AT ZERO, and there is no MAX_HARD_REJECTS.
#
# The earlier ceiling of 64 was drawn around a population -- 3 on non-boss
# levels, 6/7/14 on the boss courses -- believed to be "genuine
# mis-associations" from a registration site that composed differently from the
# list it wrote. That attribution was wrong. Every one of those rejects was a
# DEAD TENANT: the registry keys by Mtx pointer, three slot-2 pushers in
# camera.c rebuild a different matrix at an already-registered address without
# registering, and the lookup was answering with the previous tenant's world.
# Measured on level 40, every hard reject reported `bytesmoved=1` -- the bytes
# at the key had changed since registration -- and the worlds being served were
# track-tile geometry on a 960-unit grid at y = -43.
#
# With the stale-tenant guard refusing those, and the vehicle-part pusher
# registering its own tenancy, the count is 0 on every level swept (0, 5, 11,
# 17, 20, 21, 26, 38, 40, 46) and `mtxrejectleast` reports -1: there is nothing
# left to reject. So the bound is equality at zero, which is 64x tighter than
# what it replaces, and the control that the old non-zero assertion provided
# now lives in check_tenancy_control() as a deterministic injected mismatch.
# MIN_CONTROL_REJECT_LSB: the POSITIVE CONTROL injects one 2,000,000-LSB
# verification error after disabling the stale-tenant guard. The lower bound is
# 1,000,000 LSBs (~15 world units), still 244x above the production tolerance,
# so a widened threshold cannot make the control pass accidentally.
MIN_CONTROL_REJECT_LSB = 1_000_000
# MAX_TOLERATED_LSB: the other side. The worst mismatch the tolerance ACCEPTED,
# measured across those 21 levels, ranged 30-193 LSBs -- 0.0005 to 0.003 world
# units -- and 320 LSBs (0.005 units) on the PAL arms, whose 25 Hz tick makes the
# interpolated cameras further apart. Either way the float-rounding population
# never came close to the 4096 threshold it is judged against. 4096 is the
# threshold itself, so asserting the accepted worst stays under 1024 keeps a 4x
# reserve INSIDE the tolerance: if the accepted tail ever climbs into it,
# something other than rounding is being accepted and this says so before the
# threshold does.
MAX_TOLERATED_LSB = 1024
# MIN_SEPARATION: the emptiness of the gap, asserted directly. The injected
# 2,000,000-LSB mismatch is over 35,000x the current accepted worst of 56. A
# threshold is only principled if it sits in a region with nothing in it, and
# this is the assertion that the region is still empty -- it fails if either
# population ever drifts toward the other, whichever one moves.
MIN_SEPARATION = 1000

SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
REPLAY_RE = re.compile(r"\[REPLAY-SUMMARY\] (.*)")
SNAPSHOT_RE = re.compile(r"\[SNAPSHOT\] (.*)")
PACKET_RE = re.compile(r"\[PRESENT-PACKET\] (.*)")
LOAD_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+)")


class Arm:
    """One breadth entry: what to load, and which 12.3 clause it answers."""

    def __init__(self, name: str, clause: str, track: str,
                 script: Path = SCRIPT, ticks: int = TICKS,
                 rate: int = 60, rom: str | None = None,
                 humans: int = 1, expects_tolerance: bool = True):
        self.name = name
        self.clause = clause
        self.track = track
        self.script = script
        self.ticks = ticks
        self.rate = rate
        self.rom = rom
        self.humans = humans
        # Does this content produce float-re-association mismatches at all?
        # Every arm with drawn racers does, in the thousands; the low-reject
        # levels the sweep found (21, 22, 34, 39, 42-44, 47-51, 56, 63, 64) do
        # not, and asserting a non-zero tolerance count there would fail on
        # content that simply has nothing for the tolerance to do. Flagged per
        # arm from measurement rather than inferred from a count at runtime,
        # so an arm that STOPS producing them is a failure and not a shrug.
        self.expects_tolerance = expects_tolerance

    @property
    def level(self) -> int:
        return int(self.track.split(":")[0])

    @property
    def load_players(self) -> int:
        """The value `level_load` traces for `humans` human players.

        LevelHeader's numPlayers is zero-based: a one-player race traces
        `numPlayers=0` and a four-player split screen traces `numPlayers=3`.
        The menu/cutscene loads on the way in trace -1. Asserting the raw
        human count here is what made this gate's first run report that the
        4P arm "never loaded", when it had.
        """
        return self.humans - 1


def ntsc_arms() -> list[Arm]:
    return [
        # "standard AI races" + the reference the matrix gate already runs, kept
        # so a divergence between the two gates is attributable.
        Arm("ancient-lake-car", "standard AI race, car", "5"),
        # "representative car, hovercraft, and plane races". The vehicle suffix
        # is load-bearing: without it MDKR_LOAD_TRACK retargets the LEVEL only
        # and the menu route's car is kept, so an unsuffixed sweep drives all
        # twenty tracks in a car (see check_track_sweep's note).
        Arm("jungle-falls-hover", "hovercraft race", "29:1"),
        Arm("spaceport-plane", "plane race", "15:2"),
        Arm("crescent-hover", "hovercraft race, second world", "28:1"),
        # "battle and all challenge types" -- the same four courses
        # check_challenge_modes drives: eggs, bananas, and both battles.
        Arm("challenge-eggs", "challenge: eggs", "11"),
        Arm("challenge-bananas", "challenge: treasure bananas", "25"),
        Arm("challenge-battle-1", "challenge: battle", "26"),
        Arm("challenge-battle-2", "challenge: battle", "27"),
        # "Bubbler and every other boss" -- the boss courses the existing boss
        # gates name. The full 63-level sweep covers the rest.
        Arm("boss-tricky", "boss", "38"),
        Arm("boss-bluey", "boss", "40"),
        Arm("boss-smokey", "boss", "46"),
        # Historical pre-tolerance endpoints retained as numeric stress arms.
        Arm("worst-reject", "pre-tolerance worst (22.2%; now zero hard rejects)", "17"),
        # Level 21 draws no racers, so it produces NO float-re-association
        # mismatches at all -- its whole pre-tolerance reject count was the 3
        # genuine mis-associations. It is the arm that shows the hard-reject
        # count is content-independent, and the one arm where a zero tolerance
        # count is the correct answer rather than a broken tolerance.
        Arm("best-reject", "pre-tolerance best (0.012%; now zero hard rejects)", "21",
            expects_tolerance=False),
        # "1P through 4P". Every gate run before this branch was 1P, and the
        # design doc's R2 (multi-viewport) was recorded as structurally
        # addressed but UNVERIFIED. This arm is the verification: four
        # viewports, one simulation pair, one alpha.
        Arm("four-player", "1P-4P: four-way split screen", "5",
            script=SCRIPT_4P, ticks=TICKS_4P, humans=4),
    ]


def pal_arms(pal_rom: str) -> list[Arm]:
    return [
        # "NTSC and PAL". A 50 Hz policy gives this content-breadth gate the
        # same two presentation opportunities per tick as its NTSC arms.
        # PAL 60's exact 2.4 opportunities/tick has its own clock/stream gate.
        Arm("pal-ancient-lake", "PAL: standard AI race", "5",
            rate=50, rom=pal_rom),
        Arm("pal-boss", "PAL: boss", "38", rate=50, rom=pal_rom),
        Arm("pal-battle", "PAL: battle", "26", rate=50, rom=pal_rom),
    ]


def find_pal_rom(directory: Path) -> Path | None:
    """The pal.v80 release, by the same rule check_simulation_cadence uses."""
    if not directory.is_dir():
        return None
    for path in sorted(directory.iterdir()):
        name = path.name.lower()
        if ("europe" in name or "pal" in name) and name.endswith(".z64"):
            return path
    return None


def parse_row(text: str, pattern: re.Pattern[str]) -> dict[str, int]:
    match = None
    for match in pattern.finditer(text):
        pass
    if match is None:
        return {}
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, _, value = token.partition("=")
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def run(binary: Path, rom: Path, label: str, root: Path, arm: Arm,
        extra: dict[str, str], frames: int, timeout: int,
        verbose: bool) -> str:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_LOAD_TRACK=arm.track,
        MDKR_RENDERER="gl",
        # The subloop arms the snapshot store implicitly, but only the env seam
        # registers the exit report this gate reads its overflow count from.
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_SAVE_DIR=str(save_dir),
    )
    env.update(extra)
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(arm.script), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ ({label}) MDKR_LOAD_TRACK={arm.track} "
              f"{' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n"
                           f"{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    return output


def sim_hash_rows(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("[SIMHASH]")]


def check_arm(binary: Path, rom: Path, root: Path, arm: Arm, timeout: int,
              verbose: bool) -> tuple[list[str], list[str]]:
    """(failures, notes) for one breadth entry."""
    failures: list[str] = []
    ticks = arm.ticks
    base = run(binary, rom, f"{arm.name}-base", root, arm, {}, ticks,
               timeout, verbose)
    high = run(binary, rom, f"{arm.name}-rate", root, arm,
               {"MDKR_PRESENT_RATE": str(arm.rate),
                "MDKR_INTERNAL_TEST_TOKEN":
                    "mdkr64-presentation-replay-v1",
                "MDKR_PRESENT_SMOOTHING": "interpolate",
                "MDKR_TEST_PRESENTATION_REPLAY": "1",
                "MDKR_PRESENT_SCHED_TRACE": "1",
                "MDKR_TEST_ENDPOINT_VERTEX_BYTES": "1"},
               ticks * PRESENTS_PER_TICK, timeout, verbose)

    base_rows = sim_hash_rows(base)
    high_rows = sim_hash_rows(high)
    prefix = f"{arm.name} ({arm.clause})"

    # The retargeted race must actually have loaded, or the arm compared menus.
    # An unloaded level is the failure mode that made an earlier version of this
    # sweep report a clean pass on content it never reached.
    loaded = any(int(level) == arm.level and int(players) == arm.load_players
                 for level, players in LOAD_RE.findall(high))
    if not loaded:
        failures.append(
            f"{prefix}: level {arm.level} never loaded as a race with "
            f"numPlayers={arm.load_players} ({arm.humans} human player(s)); "
            "this arm compared the menu route, not its content")
    if ticks <= RACE_LOAD_TICK:
        failures.append(
            f"{prefix}: tick budget {ticks} does not outlast the route's race "
            f"load at tick {RACE_LOAD_TICK}")

    for label, rows in (("base", base_rows), ("rate", high_rows)):
        if len(rows) != ticks:
            failures.append(
                f"{prefix}: {label} produced {len(rows)} [SIMHASH] rows, "
                f"expected {ticks}. The presentation rate changed how many "
                "authoritative ticks ran, which is the one thing it must "
                "never do")
    if len(base_rows) == len(high_rows):
        for index, (a, b) in enumerate(zip(base_rows, high_rows)):
            if a != b:
                failures.append(
                    f"{prefix}: MDKR_PRESENT_RATE={arm.rate} diverges from the "
                    f"unset stream at tick {index} ({a} vs {b}) -- the "
                    "presentation rate reached authoritative state on this "
                    "content")
                break

    sched = parse_row(high, SUMMARY_RE)
    replay = parse_row(high, REPLAY_RE)
    snapshot = parse_row(high, SNAPSHOT_RE)
    packet = parse_row(high, PACKET_RE)
    if not sched or not replay or not packet:
        failures.append(f"{prefix}: no presentation telemetry was emitted")
        return failures, []

    if sched.get("presents") != ticks * PRESENTS_PER_TICK:
        failures.append(
            f"{prefix}: presented {sched.get('presents')} frames for {ticks} "
            f"ticks, expected {ticks * PRESENTS_PER_TICK} -- the subloop is "
            "not running at the requested rate on this content")
    if sched.get("simticks") != ticks:
        failures.append(
            f"{prefix}: advanced {sched.get('simticks')} authoritative ticks, "
            f"expected {ticks}")
    for key in ("multidue", "lead", "catchup", "skips", "rebases"):
        if sched.get(key, 0) != 0:
            failures.append(
                f"{prefix}: {key}={sched.get(key)} -- the accumulator left the "
                "fixed 2-presents-per-tick schedule under synthetic pacing")
    if sched.get("interp", 0) == 0:
        failures.append(
            f"{prefix}: no interpolated present was issued at all -- the "
            "subloop engaged but never drew an intermediate frame")

    # Interpolation continuity: presents that drew nothing, and the walk/restore
    # bookkeeping that has to account for every one that did.
    stale = sched.get("stale", 0)
    if stale > MAX_STALE_RATIO * ticks:
        failures.append(
            f"{prefix}: {stale} of {ticks} interpolated presents drew nothing "
            f"({stale / ticks:.2%}, bound {MAX_STALE_RATIO:.0%}) -- "
            "interpolation is dropping out on this content")
    # Successful replay walks now include the alpha-zero retained endpoint at
    # each fresh tick as well as the intermediate presents counted by `interp`.
    # The endpoint count is bounded by the number of ticks; stage-transition
    # passes with no fresh display list correctly contribute neither.
    replay_walks = replay.get("walks", 0)
    interp_walks = sched.get("interp", 0)
    endpoint_walks = replay_walks - interp_walks
    if endpoint_walks <= 0 or endpoint_walks > ticks:
        failures.append(
            f"{prefix}: {replay_walks} replay walks against {interp_walks} "
            f"interpolated presents imply {endpoint_walks} retained endpoint "
            f"walks, expected 1..{ticks}")
    if replay.get("restores") != replay.get("walks"):
        failures.append(
            f"{prefix}: {replay.get('restores')} registry restores for "
            f"{replay.get('walks')} replays -- the frozen registry is not "
            "reaching every replay")
    if replay.get("freezefail", 0) != 0 or replay.get("restorefail", 0) != 0:
        failures.append(
            f"{prefix}: {replay.get('freezefail')} freeze and "
            f"{replay.get('restorefail')} restore/release failures -- the "
            "registry lost a frame of registrations")
    if snapshot.get("overflows", 0) != 0:
        failures.append(
            f"{prefix}: {snapshot.get('overflows')} snapshot captures failed "
            "whole; the published pair went stale for those ticks")
    if snapshot and snapshot.get("captures", 0) != ticks:
        failures.append(
            f"{prefix}: {snapshot.get('captures')} snapshot captures for "
            f"{ticks} ticks -- capture is not running once per tick")
    if packet.get("freezefail", 0) != 0:
        failures.append(
            f"{prefix}: {packet.get('freezefail')} retained presentation "
            "packet freezes failed whole -- a capacity/allocation limit held "
            "the authored image")
    if packet.get("unsafestalefallback", -1) != 0:
        failures.append(
            f"{prefix}: replay used {packet.get('unsafestalefallback')} "
            "rewritten live matrix/anchor dependencies instead of frozen "
            "real-walk bytes")
    if (packet.get("stale", 0) > 0 and
            packet.get("stalematrixhold", 0) +
            packet.get("stalevertexhold", 0) <= 0):
        failures.append(
            f"{prefix}: {packet.get('stale')} rewritten dependencies were "
            "detected but none were retained safely")
    if packet.get("deformcollision", 0) != 0:
        failures.append(
            f"{prefix}: {packet.get('deformcollision')} retained deformation "
            "keys were ambiguous -- this content needs a more specific stable "
            "root-stream recipe")
    if packet.get("deformreg", 0) <= 0 or packet.get("deformpeak", 0) <= 0:
        failures.append(
            f"{prefix}: deformation packet never captured model vertices "
            f"(registrations={packet.get('deformreg')}, "
            f"peak={packet.get('deformpeak')})")
    if packet.get("deformhold", 0) <= 0:
        failures.append(
            f"{prefix}: retained vertex fallback held no exact authored "
            "bytes; replay may be reading a reused arena pointer")
    if packet.get("deformphasehold", 0) <= 0:
        failures.append(
            f"{prefix}: retained vertices recorded no task-to-next phase gap; "
            "the gate cannot prove lagged pairs are refused")
    for key in ("deformhit", "deformoverride", "colorhit",
                "coloroverride"):
        if packet.get(key, -1) != 0:
            failures.append(
                f"{prefix}: retained model {key}={packet.get(key)}, expected "
                "zero; replay used a phase-shifted endpoint pair")
    if (packet.get("endpointchecks", 0) <= 0 or
            packet.get("endpointmismatch", -1) != 0 or
            packet.get("endpointexpected", 0) == 0 or
            packet.get("endpointexpected") != packet.get("endpointactual")):
        failures.append(
            f"{prefix}: alpha-zero G_VTX semantics do not hash exactly to the "
            "task-authored retained endpoint")
    if arm.name == "challenge-battle-1":
        if packet.get("particlevertexreg", 0) <= 0:
            failures.append(
                f"{prefix}: battle point-trail witness captured no retained "
                "particle batches")
        for key in ("particledeformhit", "particledeformoverride",
                    "particlecolorhit", "particlecoloroverride"):
            if packet.get(key, -1) != 0:
                failures.append(
                    f"{prefix}: battle point-trail witness {key}="
                    f"{packet.get(key)}, expected zero while its true next "
                    "endpoint is unavailable")

    hits = replay.get("mtxhit", 0)
    rejects = replay.get("mtxreject", 0)
    total = hits + rejects
    ratio = rejects / total if total else 0.0
    if total == 0:
        failures.append(
            f"{prefix}: zero gameplay matrices were recomposed -- the "
            "interpolation path this arm exists to exercise did not run")
    elif ratio > MAX_REJECT_RATIO:
        failures.append(
            f"{prefix}: {rejects}/{total} ({ratio:.1%}) of gameplay matrices "
            f"failed to recompose and kept the display list's own matrix, "
            f"over the {MAX_REJECT_RATIO:.0%} bound. Those objects hold the "
            "TICK's camera while the rest of the scene gets the interpolated "
            "one, so the frame mixes two cameras; and because the fallback is "
            "bit-exact, no hash or pixel assertion can see it")

    # ---- the recomposition tolerance, per arm ------------------------------
    #
    # (a) the hard-reject count has collapsed to the genuine mis-associations
    #     and the tolerance is actually carrying the rest;
    # (b) the genuine ~2,320-world-unit mis-associations are STILL rejected --
    #     the positive control that the tolerance did not swallow them;
    # (c) [SIMHASH] byte-identity, which the stream comparison above already
    #     asserts for this arm: presentation stays non-authoritative whether a
    #     matrix is accepted bit-exactly, accepted by tolerance, or rejected.
    tolerated = replay.get("mtxtol", 0)
    tol_worst = replay.get("mtxtolworst", 0)
    reject_least = replay.get("mtxrejectleast", -1)
    if "mtxtol" not in replay:
        failures.append(
            f"{prefix}: [REPLAY-SUMMARY] carries no mtxtol field -- the "
            "recomposition tolerance is not in this build, so none of the "
            "presentation-quality bounds below proved anything")
    else:
        # (b) The hard-reject population is EXTINCT BY DESIGN, so this is now
        # an equality and not a ceiling. Every hard reject ever measured on
        # this gate was a dead tenant -- a registry entry whose Mtx address had
        # been rewritten by a later, unregistered slot-2 push -- and the
        # stale-tenant guard refuses those instead of recomposing them. There
        # is no remaining population of "genuine mis-associations" to be
        # tolerant of, so anything above zero is a NEW defect.
        #
        # This deliberately replaces the old MAX_HARD_REJECTS=64 ceiling and
        # the old "rejects must be non-zero" positive control. That control
        # asserted the tolerance had not swallowed the genuine population, and
        # it can no longer be stated on this arm because the population is
        # gone. It is not dropped: it MOVES to check_tenancy_control(), which
        # disables the guard to prove that switch and independently injects a
        # 2,000,000-LSB verification error. Asserting zero here while proving
        # elsewhere that the tolerance still refuses a gross geometric error
        # is strictly stronger than the pair it replaces.
        if rejects != 0:
            failures.append(
                f"{prefix}: {rejects} matrices were rejected outright; the "
                "bound is ZERO. Every hard reject measured on this content was "
                "a dead tenant, and the stale-tenant guard refuses those "
                "before they reach the recomposition. A reject here is a "
                "decomposition that is wrong by more than "
                f"{MAX_TOLERATED_LSB} LSBs on a LIVE registry entry -- either "
                "a new registration site composes differently from the list it "
                "writes, or the guard has stopped firing")
        if reject_least >= 0:
            failures.append(
                f"{prefix}: a rejected magnitude of {reject_least} LSBs was "
                "reported on an arm that must reject nothing at all")
        # (a) the other half: the tolerance is carrying the population the
        # hard-reject collapse is supposed to have moved. Without this, a
        # tolerance that had stopped firing entirely would show a hard-reject
        # count of thousands -- caught above -- but a tolerance disabled at the
        # same time as the sites that feed it would look clean.
        if arm.expects_tolerance and tolerated == 0:
            failures.append(
                f"{prefix}: the tolerance accepted ZERO matrices on content "
                "measured to produce thousands of float-re-association "
                "mismatches. Either the vp_overridden gating stopped matching "
                "the interpolated path, or these matrices are being rejected "
                "again -- in which case this arm's geometry is back to being "
                "drawn with the tick's camera while the rest of the frame is "
                "not")
        if tolerated and tol_worst > MAX_TOLERATED_LSB:
            failures.append(
                f"{prefix}: the largest ACCEPTED mismatch is {tol_worst} LSBs "
                f"({tol_worst / 65536:.4f} world units), over the "
                f"{MAX_TOLERATED_LSB} bound. The tolerance exists for float "
                "re-association, measured at 30-186 LSBs; something an order "
                "of magnitude coarser is being accepted and drawn")
        # MIN_SEPARATION needs BOTH populations and one of them is extinct on
        # this arm by design, so it is asserted in check_tenancy_control()
        # instead, where the guard is disabled and the rejected population
        # exists again. It is not weakened -- it is stated where it is still
        # measurable.

    if arm.humans >= 2:
        # Spec 7: all viewports share one simulation pair and one alpha, so the
        # only thing that may vary per viewport is the view-projection. This
        # asserts more than one was actually substituted -- design doc R2.
        views = sched.get("interpviews", 0)
        interp = sched.get("interp", 1)
        per_present = views / interp if interp else 0.0
        if per_present <= 1.0:
            failures.append(
                f"{prefix}: {views} substituted view-projections over {interp} "
                f"interpolated presents ({per_present:.2f}/present) -- a "
                "multi-viewport arm that substituted at most one camera per "
                "present is not exercising the per-viewport path")

    note = (f"{arm.name:20s} {arm.clause:38s} "
            f"hardreject={rejects} (least {reject_least} LSB) "
            f"tolerated={tolerated} (worst {tol_worst} LSB) "
            f"reject={ratio:6.2%} ({rejects}/{total})  "
            f"stale={stale:<4d} interp={sched.get('interp')}  "
            f"views/present={sched.get('interpviews', 0) / max(1, sched.get('interp', 1)):.2f}  "
            f"snapshot: {snapshot.get('captures', 0)} captures, "
            f"{snapshot.get('discontinuities', 0)} discontinuities, "
            f"{snapshot.get('overflows', 0)} overflows, "
            f"{snapshot.get('resets', 0)} resets; "
            f"deformation: {packet.get('deformreg', 0)} captures, "
            f"{packet.get('deformhold', 0)} exact holds, "
            f"{packet.get('deformphasehold', 0)} phase gaps, "
            f"{packet.get('endpointchecks', 0)} exact alpha-zero checks, "
            f"{packet.get('deformcollision', 0)} collisions, "
            f"peak {packet.get('deformpeak', 0)}; "
            f"rewritten dependencies: {packet.get('stale', 0)} observed, "
            f"{packet.get('stalematrixhold', 0)} matrix and "
            f"{packet.get('stalevertexhold', 0)} anchor holds, "
            f"particles: {packet.get('particlevertexreg', 0)} batches, "
            f"{packet.get('particledeformhit', 0)} lagged blends")
    return failures, [note]


def check_tenancy_control(binary: Path, rom: Path, root: Path, timeout: int,
                         verbose: bool) -> tuple[list[str], list[str]]:
    """The POSITIVE CONTROL for the stale-tenant guard and the tolerance.

    Every content arm above asserts ZERO hard rejects. On its own that is a
    weak statement: a tolerance that had grown until it accepted everything
    would satisfy it just as well as a guard that correctly refuses dead
    tenants. The two mechanisms are checked independently here.

    ``MDKR_SHADOW_TENANCY=0`` restores the pre-fix lookup -- serve the first
    tenant's binding without checking that it still describes what is at the
    key -- on the SAME binary, one env var apart. Owner-specific interpolation
    can now supersede those stale worlds before recomposition, so allocator
    reuse is no longer a deterministic rejection injector. The control pairs
    the switch with ``MDKR_TEST_RECOMPOSE_REJECT=1``, which moves one stack-local
    verification element by exactly 2,000,000 LSBs. It therefore proves both
    mechanisms without depending on a stale binding remaining visually live:

      * the production guard still fires on natural dead tenants, and the
        control switch really disables its counter; and
      * the tolerance still rejects a geometric error far above its 4,096-LSB
        acceptance band.

    It also asserts the guard actually FIRES on this content, so a build where
    the tenancy check silently became a no-op fails here rather than passing
    everything.

    Boss content is used because it still produces the in-race dead-tenant burst
    (24 guard refusals on the current level-38 route).
    """
    failures: list[str] = []
    notes: list[str] = []
    arm = Arm("tenancy-control", "positive control: guard disabled", "38")
    frames = arm.ticks * PRESENTS_PER_TICK

    guarded = run(binary, rom, "tenancy-on", root, arm,
                  {"MDKR_PRESENT_RATE": str(arm.rate),
                   "MDKR_INTERNAL_TEST_TOKEN":
                       "mdkr64-presentation-replay-v1",
                   "MDKR_PRESENT_SMOOTHING": "interpolate",
                   "MDKR_TEST_PRESENTATION_REPLAY": "1",
                   "MDKR_PRESENT_SCHED_TRACE": "1"},
                  frames, timeout, verbose)
    control_out = run(binary, rom, "tenancy-off", root, arm,
                      {"MDKR_PRESENT_RATE": str(arm.rate),
                       "MDKR_INTERNAL_TEST_TOKEN":
                           "mdkr64-presentation-replay-v1",
                       "MDKR_PRESENT_SMOOTHING": "interpolate",
                       "MDKR_TEST_PRESENTATION_REPLAY": "1",
                       "MDKR_PRESENT_SCHED_TRACE": "1",
                       "MDKR_SHADOW_TENANCY": "0",
                       "MDKR_TEST_RECOMPOSE_REJECT": "1"},
                      frames, timeout, verbose)

    on = parse_row(guarded, REPLAY_RE)
    off = parse_row(control_out, REPLAY_RE)
    prefix = "tenancy-control"

    if "staletenants" not in on:
        failures.append(
            f"{prefix}: [REPLAY-SUMMARY] carries no staletenants field -- the "
            "stale-tenant guard is not in this build, so the zero-reject "
            "bound every other arm asserts proves nothing")
        return failures, notes

    stale = on.get("staletenants", 0)
    if stale == 0:
        failures.append(
            f"{prefix}: the guard refused ZERO dead tenants on content "
            "measured to produce 24 of them. Either the slot-2 pushers stopped "
            "re-using registered matrix addresses, or the tenancy check has "
            "become a no-op -- and the second silently restores wrong "
            "shadow-caster worlds while every other arm still passes")
    if on.get("mtxreject", -1) != 0:
        failures.append(
            f"{prefix}: {on.get('mtxreject')} hard rejects with the guard "
            "ENABLED; this arm must match the others at zero")

    # The off arm must prove the switch itself, independently of the injected
    # tolerance failure below.
    off_stale = off.get("staletenants", -1)
    if off_stale != 0:
        failures.append(
            f"{prefix}: MDKR_SHADOW_TENANCY=0 still reported {off_stale} "
            "stale tenants; the broken-direction switch no longer disables "
            "the guard it exists to test")

    # The deterministic verification error must be rejected. Natural dead
    # tenants are no longer used for this because owner-specific replay can
    # safely supersede their stale worlds before recomposition.
    off_rejects = off.get("mtxreject", 0)
    off_least = off.get("mtxrejectleast", -1)
    off_tol_worst = off.get("mtxtolworst", 0)
    if off_rejects == 0:
        failures.append(
            f"{prefix}: MDKR_TEST_RECOMPOSE_REJECT=1 produced ZERO hard "
            "rejects; the 2,000,000-LSB error was swallowed, so the production "
            "zero-reject bound no longer proves the tolerance is selective")
    elif off_least < MIN_CONTROL_REJECT_LSB:
        failures.append(
            f"{prefix}: the smallest rejected mismatch is "
            f"{off_least} LSBs ({off_least / 65536:.3f} world units), under "
            f"the {MIN_CONTROL_REJECT_LSB} control bound; the injected error "
            "did not reach the comparison as specified")
    elif off_tol_worst > 0:
        separation = off_least / off_tol_worst
        if separation < MIN_SEPARATION:
            failures.append(
                f"{prefix}: accepted-worst {off_tol_worst} and rejected-least "
                f"{off_least} are only {separation:.0f}x apart (bound "
                f"{MIN_SEPARATION}x). The two populations have stopped being "
                "separable, so ANY threshold between them is now a judgement "
                "call rather than a measurement")
        else:
            notes.append(
                f"{prefix}: guard on -> {stale} dead tenants refused, 0 hard "
                f"rejects; guard off + injected error -> {off_rejects} hard "
                f"rejects, least "
                f"{off_least} LSBs ({off_least / 65536:.0f} world units), "
                f"{separation:.0f}x above the worst tolerated")

    if sim_hash_rows(guarded) != sim_hash_rows(control_out):
        failures.append(
            f"{prefix}: the [SIMHASH] streams differ between the guarded and "
            "unguarded arms. The registry feeds shadows and presentation only "
            "-- if refusing a dead tenant moves authoritative state, something "
            "reads the caster path back into the simulation")

    return failures, notes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--roms", default="build/roms",
                        help="directory searched for the pal.v80 release")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--only", default=None,
                        help="comma-separated arm names (iteration only)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT, SCRIPT_4P):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    pal_rom = find_pal_rom(Path(os.path.abspath(args.roms)))
    if pal_rom is None:
        print(f"check_presentation_breadth: FAIL\n  - spec 12.3 requires NTSC "
              f"AND PAL, and no PAL v80 release was found below {args.roms}. "
              "This gate does not silently skip its region arm.")
        return 1

    arms = ntsc_arms() + pal_arms(str(pal_rom))
    if args.only:
        wanted = {name.strip() for name in args.only.split(",")}
        arms = [arm for arm in arms if arm.name in wanted]

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-present-breadth-") as tmp:
        root = Path(tmp)
        for arm in arms:
            arm_rom = Path(arm.rom) if arm.rom else rom
            try:
                arm_failures, arm_notes = check_arm(
                    binary, arm_rom, root, arm, args.timeout, args.verbose)
            except RuntimeError as error:
                failures.append(str(error))
                continue
            failures.extend(arm_failures)
            notes.extend(arm_notes)
        if not args.only or "tenancy-control" in args.only:
            try:
                control_failures, control_notes = check_tenancy_control(
                    binary, rom, root, args.timeout, args.verbose)
            except RuntimeError as error:
                failures.append(str(error))
            else:
                failures.extend(control_failures)
                notes.extend(control_notes)
    if failures:
        print("check_presentation_breadth: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    scope = ("selected content" if args.only else
             "bosses, all challenge types, car/hovercraft/plane, 1P and 4P, "
             "NTSC and PAL")
    print(f"check_presentation_breadth: PASS -- [SIMHASH] byte-identical with "
          f"the presentation subloop engaged over {len(arms)} content arms "
          f"({scope})")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
