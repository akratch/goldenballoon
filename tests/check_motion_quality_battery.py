#!/usr/bin/env python3
"""The perceptual failure modes of interpolated presentation, one row each.

Motion smoothing reconstructs three of every four images a player sees on a
120 Hz display. The tree already has gates for the *mechanism* of that
reconstruction -- which stage moved what, whether a recipe was anchored to its
own tick, whether an ownership lookup refused. What it did not have until now
is a single gate that states, in the vocabulary a player would use, the things
a reconstruction is never allowed to do:

    a kart may not spin the wrong way round;
    a respawn may not be smeared across the screen;
    a camera cut may not be blended through;
    time may not run backwards;
    the authored tick may not thump.

Those are rows R1..R6 below. Their taxonomy, the measurements that named them
and the dispositions that closed them are in
``docs/evidence/smoothing-artifact-repro-2026-08.md`` -- classes C1..C9, the
gate design in §2.6, the dispositions in §5. This gate is the standing form of
that note: the release gate future presentation work has to keep green.

Every row has a NEGATIVE CONTROL
--------------------------------

A gate that has only ever been green proves that it ran, not that it can fail.
So each row that drives a scenario also runs a committed, token-gated arm that
provably turns it red, in the same run, on the same route, and the gate FAILS
if that arm comes back green. The controls are code (``presentation_snapshot.c``,
seam block at the top), not prose:

* R1 ``MDKR_TEST_ROTATION_LONG_ARC`` -- every interpolated angle takes the long
  way round the circle instead of the short one. Artifact class C3.
* R2 ``MDKR_TEST_IGNORE_DISCONTINUITY`` -- the spawn/teleport/cut flag stops
  being honoured, so replay blends across a respawn. The object-side sibling of
  class C2.

Both are inert without ``MDKR_INTERNAL_TEST_TOKEN``, both are presentation-only
by construction, and this gate proves the second claim rather than asserting
it: every control arm's ``[SIMHASH]``/``[EVENTHASH]``/``[INPUTHASH]`` stream
must be byte-identical to the production arm it is a control for. A seam that
moved a hash would be a real defect in the seam, and is reported as one.

What this gate deliberately does NOT re-drive
---------------------------------------------

Two rows are cross-references. ``check_camera_snapshot_coverage.py`` already
classifies camera cuts out of raw poses and demands the snapshot refused to
pair across each one; ``check_effect_shell_envelope.py`` already holds the
effect shell inside its endpoint-to-endpoint envelope. Re-driving either here
would double the suite's cost to re-derive a verdict that already exists.

So R3 and R6 assert something cheaper and, for a consolidating battery, more
load-bearing: that the referenced gate is still REGISTERED in
``tools/run_checks.py``'s manifest, in a role the release suite runs. That is
what keeps a citation honest. The suite runner fails closed on any
``tests/check_*.py`` that is not registered, and runs every check that is --
so "registered in a release role" plus the suite's own verdict is exactly
"green", while a future consolidation that deleted or demoted one of them would
break this gate's citation instead of silently un-covering the class.

The stat contract
-----------------

Every counter this gate reads is checked for PRESENCE before its value is read.
A dict lookup with a default silently passes the day a counter is renamed,
which turns the strongest claim a gate makes into a no-op. Two of the rows
below assert a zero, and a zero that comes from a missing field looks exactly
like a zero that comes from correct behaviour -- so each of those also asserts
its own non-vacuity witness, a companion counter that must be non-zero for the
zero to mean anything.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import math
import operator
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from harness_utils import (DEFAULT_BUILD_DIR, parse_last, parse_rows,
                           resolve_binary, text_rows)

ROOT = Path(__file__).resolve().parent.parent
HASH_VERSION = "3"

# The versioned token every adversarial presentation seam requires
# (present_sched.c, and presentation_snapshot.c's own copy of the literal).
# Without it both negative controls below are inert, which is the point: a
# stray MDKR_TEST_* in a developer's shell cannot arm one.
INTERNAL_TEST_TOKEN = "mdkr64-presentation-replay-v1"

# The 120 Hz alpha grid: the authoritative tick is two 60 Hz fields, so four
# presents per tick -- the authored endpoint plus interpolated images at alpha
# 1/4, 2/4, 3/4. This is the densest grid production can request, and every
# artifact in the note is at its worst here.
PRESENT_RATE = "120"
PRESENTS_PER_TICK = 4

# ---------------------------------------------------------------------------
# R1/R2/R4 -- the spin route.
#
# race_full_3lap.txt on level 29 under autopilot: three full laps, so the
# racers corner under power, collide, spin out and are respawned by the
# recovery logic, and the chase camera swings through all of it. It is chosen
# over the diagnosis note's shorter navigation route for one measured reason:
# it is the only route probed that produces past-quarter-turn rotation deltas
# at all (rotarcsnap=18 against 0 on nav_to_time_trial_race, the battle
# challenge, and race_drive_long), which is the population R1's snap clause
# exists to grade. A route with no fast rotation would let R1 pass by having
# nothing to fail on.
SPIN_SCRIPT = ROOT / "tests" / "input_scripts" / "race_full_3lap.txt"
SPIN_TRACK = "29"
SPIN_TICKS = 6000

# ---------------------------------------------------------------------------
# R5 -- the diagnosis route, verbatim.
#
# Level 29 Jungle Falls, nav_to_time_trial_race.txt, authored ticks 3200-3230,
# forced shield. Same route, window and instrument as
# docs/evidence/smoothing-artifact-repro-2026-08.md §3 and §5.1, because R5's
# threshold is calibrated from figures measured on exactly this arm and a
# threshold is only meaningful against the measurement it came from.
BOUNDARY_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
BOUNDARY_TRACK = "29"
BOUNDARY_TICKS = 3230
BOUNDARY_DUMP_FRAMES = 120
# MDKR_FORCE_SHIELD is expressed in AUTHORED TICKS; the hook counts
# g_simTickCounter so the window no longer has to be rescaled per present rate.
# It used to count presents, and the 2026-08 diagnosis lost a whole 60 Hz arm
# (effectreg=0) to exactly that trap.
BOUNDARY_FORCE_SHIELD = "3170:150"

# ---------------------------------------------------------------------------
# R4 -- the displayed alpha grid, in ppm of one authoritative tick.
#
# present_sched.c differences the monotone presentation phase between
# consecutive DISPLAYED presents. On a four-present grid the only legal
# advance is a whole number of quarter ticks: 250,000 ppm normally, and a
# multiple of it after a held present, because a hold's quarter tick of
# advance is not lost -- it lands in the following interval. That correction
# is §5.4's, and an earlier draft of the note got it wrong in the strict
# direction ("every present advances exactly one quarter tick"), which the
# max=1,250,000 tail disproves.
ALPHA_SCALE_PPM = 1_000_000
GRID_STEP_PPM = ALPHA_SCALE_PPM // PRESENTS_PER_TICK

# ---------------------------------------------------------------------------
# R5 -- the tick-boundary step bound, and where the number comes from.
#
# "Boundary excess" is the mean absolute inter-frame step INTO an authored
# endpoint, measured against the mean of the three steps inside a tick. On a
# uniform alpha grid, content every stage covers moves the same distance on
# each of the four steps, so the reference is 0% and anything above it is
# content advancing only at the authored tick.
#
# The calibration (note §5.1, same route, same window). Note §5.1 reports two
# forms of this statistic and they are NOT interchangeable: the POOLED form
# averages all boundary steps against all interior steps in the window, while
# the PAIRED form uses only ticks that contributed all four of their steps.
# This gate computes the PAIRED form, because the hold guard below drops
# contaminated ticks whole and a tick that contributed three interiors but no
# boundary step would otherwise put a full step next to a mean taken over
# fewer:
#
#   shipped 1.0.1-1.0.3 production, with the C1 anchoring defect   +16.68% pooled
#   fixed build, production                                         +3.73% pooled
#   fixed build, production, PAIRED -- what this gate computes      +2.93%
#   camera-only arm -- what wholly uncovered content looks like    +18.17% pooled
#
# Only the fixed build has both forms published, and the pooled figures sit
# slightly above their paired counterparts, so the historical comparison is
# indicative rather than exact. It does not have to be exact: 8% is placed in
# the empty band between the two populations, 2.7x above what the build
# actually measures and half the margin a C1-class regression reopens. It is a
# REGRESSION bound, not a quality target -- the
# residual it permits is dominated by the 2D HUD, which is authored screen-space
# texrects with no world pose pair to interpolate and reads +10.00% on its own
# whether every stage is on or every stage is off (§5.1). Tightening it toward
# 3.73% would make the gate a sampling-noise detector on a route whose non-HUD
# remainder has a 95% CI of -0.30%..+3.29%.
BOUNDARY_EXCESS_MAX = 0.08

# ---------------------------------------------------------------------------
# Stat contracts. Presence before value, everywhere.
SNAPSHOT_STAT_CONTRACT = (
    "rotarccheck", "rotarcsnap", "rotarcviolation",
    "disconthold", "discontblend")
SCHED_STAT_CONTRACT = ("presents", "stale")
ALPHA_STAT_CONTRACT = (
    "n", "min", "max", "mean", "p99", "binwidth", "regressions", "stalls",
    "presents", "displayed")

# ---------------------------------------------------------------------------
# R3/R6 -- the gates this battery consolidates around rather than duplicating.
# (row, registered check name, script, the property being cited)
CROSS_REFERENCES = (
    ("R3", "camera_snapshot_coverage", "check_camera_snapshot_coverage.py",
     "classifies camera cuts out of raw poses -- viewport entry, camera jump, "
     "mode reframe -- and demands the snapshot refused to pair across every "
     "one. Artifact class C2"),
    ("R6", "effect_shell_envelope", "check_effect_shell_envelope.py",
     "holds the shield/magnet shell's displacement from its own tick's "
     "authored pose inside the endpoint-to-endpoint envelope. Artifact class "
     "C1, and the class C6 contrast arm with it"),
)
# The roles tools/run_checks.py runs in a full native release pass. A check
# demoted out of these would still be "registered" and would stop running,
# which is the failure this tuple exists to catch.
RELEASE_ROLES = ("release", "native", "rom", "ctest")



RENDERER = "webgpu"

def run(binary: Path, rom: Path, label: str, root: Path, script: Path,
        track: str, ticks: int, extra_env: dict[str, str], dump: bool,
        timeout: int, verbose: bool) -> tuple[str, Path | None, float]:
    """One headless arm. Returns (combined output, frame dir or None, seconds)."""

    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=track,
        # GL, deliberately, and NOT because GL is what matters.
        #
        # This gate drives 120 Hz to get several interpolated presents per
        # authoritative tick. On WebGPU that is exactly the window where a
        # replay is refused admission (it needs zero frames in flight and the
        # tick's own frame has not retired at 8.3 ms), so ~80% of presents come
        # back stale and the rows below measure a stream that barely
        # interpolates -- R4 saw displayed=4868 of 24000 presents. The gate
        # would be failing on the renderer's admission policy rather than on
        # anything it exists to judge.
        #
        # Pass --renderer webgpu to run it there anyway; it is one command, and
        # it is how the starvation was characterised. Making WebGPU the default
        # is blocked on the open item in docs/open-items/renderer.md
        # ("interpolated presents are starved on WebGPU above the display
        # refresh"), not on anything in this file.
        MDKR_RENDERER=RENDERER,
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_PRESENT_RATE=PRESENT_RATE,
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        # The [SNAPSHOT] exit row is registered by the ENV seam only: replay
        # arming enables the module programmatically and never installs the
        # atexit hook, so without this the whole R1/R2 census is silently
        # absent rather than zero.
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_PRESENT_PERF="1",
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-ticks", str(ticks),
        "--input-script", str(script), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    frame_dir: Path | None = None
    if dump:
        frame_dir = run_dir / "frames"
        frame_dir.mkdir(parents=True)
        env["MDKR_DUMP_FROM"] = str(
            ticks * PRESENTS_PER_TICK - BOUNDARY_DUMP_FRAMES)
        command += ["--dump-frames", str(frame_dir)]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    started = time.monotonic()
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False)
    elapsed = time.monotonic() - started
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    return output, frame_dir, elapsed


def hash_streams(output: str) -> tuple[str, ...]:
    """The three authoritative streams, verbatim and in order."""

    return tuple(
        line for line in output.splitlines()
        if line.startswith(("[SIMHASH]", "[EVENTHASH]", "[INPUTHASH]")))


def require_fields(row: dict[str, int], contract: tuple[str, ...],
                   tag: str, label: str, source: str) -> list[str]:
    missing = [key for key in contract if key not in row]
    if not missing:
        return []
    return [
        f"{label}: {tag} carries no {'/'.join(missing)} field. That is this "
        f"gate's stat contract ({source}); without it every assertion below "
        "reads a default and passes while asserting nothing"]


def read_frame(path: Path) -> bytes:
    """The PPM payload with its header stripped, so a step is pixels only."""

    data = path.read_bytes()
    fields = 0
    offset = 0
    while fields < 4:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b"#":
            while offset < len(data) and data[offset] != 0x0A:
                offset += 1
            continue
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        fields += 1
    return data[offset + 1:]


def mean_abs_step(left: bytes, right: bytes) -> float:
    if len(left) != len(right):
        raise RuntimeError("frame size moved mid-run")
    total = 0
    for delta in map(operator.sub, left, right):
        total += delta if delta > 0 else -delta
    return total / len(left)


def alpha_delta_rows(output: str) -> list[dict[str, int]]:
    """``[PRESENTPERF-HIST] series=alpha-delta`` rows only.

    The tag carries three different series and ``parse_last`` would hand back
    whichever came last, which is a latency histogram in microseconds rather
    than a phase histogram in ppm. Selecting by the series field rather than by
    position is what stops R4 silently becoming the wrong measurement in the
    right units' clothing.
    """

    numeric = parse_rows(output, "PRESENTPERF-HIST")
    labelled = text_rows(output, "PRESENTPERF-HIST")
    return [row for row, text in zip(numeric, labelled)
            if text.get("series") == "alpha-delta"]


def registry() -> dict[str, tuple[str, str]]:
    """tools/run_checks.py's manifest, read from the runner itself.

    Imported rather than regex-scraped: the manifest is the runner's own
    dataclass tuple, and a citation that parsed a text pattern out of it would
    keep agreeing with a file that no longer says what it used to.
    """

    sys.path.insert(0, str(ROOT / "tools"))
    try:
        import run_checks  # noqa: PLC0415 - deliberately late and path-scoped
    finally:
        sys.path.pop(0)
    return {check.name: (check.script, check.role) for check in run_checks.CHECKS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("webgpu", "gl"),
                        default="gl",
                        help="backend under test (default: the shipped one)")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    global RENDERER
    RENDERER = args.renderer

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SPIN_SCRIPT, BOUNDARY_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    rows: list[str] = []
    control_env = {"MDKR_INTERNAL_TEST_TOKEN": INTERNAL_TEST_TOKEN}
    started = time.monotonic()

    # -- R3 and R6 first: they are free, and a battery whose citations have
    # rotted should say so before spending four minutes on the rest.
    try:
        manifest = registry()
    except Exception as error:  # noqa: BLE001 - reported, not swallowed
        print(f"FAIL: could not read tools/run_checks.py's manifest: {error}",
              file=sys.stderr)
        return 1
    for row, name, script, cited in CROSS_REFERENCES:
        if name not in manifest:
            rows.append(f"[{row}-CROSS-REFERENCE] check={name} verdict=FAIL")
            failures.append(
                f"{row}: {name} ({script}) is not registered in "
                f"tools/run_checks.py. This battery cites it for the property "
                f"that it {cited}, and does not re-drive that scenario because "
                "the citation was supposed to make re-driving unnecessary. An "
                "unregistered check does not run, so the class is now "
                "uncovered in both places")
            continue
        registered_script, role = manifest[name]
        if registered_script != script:
            rows.append(f"[{row}-CROSS-REFERENCE] check={name} verdict=FAIL")
            failures.append(
                f"{row}: {name} is registered as {registered_script}, but this "
                f"battery cites {script}. One of the two moved and the "
                "citation no longer points at the gate it names")
            continue
        if role not in RELEASE_ROLES:
            rows.append(f"[{row}-CROSS-REFERENCE] check={name} role={role} "
                        "verdict=FAIL")
            failures.append(
                f"{row}: {name} is registered in role '{role}', which the "
                "native release pass does not run. Registered but not run is "
                "the one state a citation cannot distinguish from covered")
            continue
        if not (ROOT / "tests" / script).exists():
            rows.append(f"[{row}-CROSS-REFERENCE] check={name} verdict=FAIL")
            failures.append(
                f"{row}: tests/{script} is registered but absent from the tree")
            continue
        rows.append(
            f"[{row}-CROSS-REFERENCE] check={name} script={script} "
            f"role={role} verdict=PASS")

    with tempfile.TemporaryDirectory(prefix="mdkr-motion-battery-") as tmp:
        root = Path(tmp)
        try:
            spin_out, _, spin_secs = run(
                binary, rom, "spin", root, SPIN_SCRIPT, SPIN_TRACK,
                SPIN_TICKS, {}, False, args.timeout, args.verbose)
            arc_out, _, arc_secs = run(
                binary, rom, "spin-longarc", root, SPIN_SCRIPT, SPIN_TRACK,
                SPIN_TICKS,
                dict(control_env, MDKR_TEST_ROTATION_LONG_ARC="1"),
                False, args.timeout, args.verbose)
            flag_out, _, flag_secs = run(
                binary, rom, "spin-noflag", root, SPIN_SCRIPT, SPIN_TRACK,
                SPIN_TICKS,
                dict(control_env, MDKR_TEST_IGNORE_DISCONTINUITY="1"),
                False, args.timeout, args.verbose)
            boundary_out, frame_dir, boundary_secs = run(
                binary, rom, "boundary", root, BOUNDARY_SCRIPT, BOUNDARY_TRACK,
                BOUNDARY_TICKS,
                {"MDKR_FORCE_SHIELD": BOUNDARY_FORCE_SHIELD}, True,
                args.timeout, args.verbose)
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            print(f"FAIL: {error}", file=sys.stderr)
            return 1

        assert frame_dir is not None
        frames = sorted(frame_dir.glob("*.ppm"),
                        key=lambda path: int(path.stem.split("_")[1]))
        if len(frames) != BOUNDARY_DUMP_FRAMES:
            print(f"FAIL: boundary arm dumped {len(frames)} frames, expected "
                  f"{BOUNDARY_DUMP_FRAMES}", file=sys.stderr)
            return 1
        indexed = [(int(path.stem.split("_")[1]), read_frame(path))
                   for path in frames]

    rows.append(
        f"[BATTERY-ARMS] spin={spin_secs:.0f}s longarc={arc_secs:.0f}s "
        f"noflag={flag_secs:.0f}s boundary={boundary_secs:.0f}s")

    # -- The seams are presentation-only, and this is where that is proved
    # rather than claimed. Done before the rows so a control arm that moved a
    # hash is reported as the defect it would be, not as a red row.
    baseline = hash_streams(spin_out)
    if not baseline:
        print("FAIL: the spin arm emitted no [SIMHASH]/[EVENTHASH]/[INPUTHASH] "
              "rows, so no control arm's stream identity can be checked and "
              "both negative controls are unconstrained", file=sys.stderr)
        return 1
    impure = 0
    for label, output in (("spin-longarc", arc_out), ("spin-noflag", flag_out)):
        stream = hash_streams(output)
        if stream != baseline:
            impure += 1
            differing = sum(1 for left, right in zip(baseline, stream)
                            if left != right)
            failures.append(
                f"{label}: the authoritative streams differ from the "
                f"production arm ({len(baseline)} rows vs {len(stream)}, "
                f"{differing} differing). Both seams are presentation-only by "
                "construction -- they change what a REPLAYED pose looks like "
                "and never touch a captured sample -- so a stream that moves "
                "is a real defect in the seam, not a harness artifact")
    rows.append(
        f"[SEAM-PURITY] hashrows={len(baseline)} arms=2 impure={impure} "
        f"verdict={'FAIL' if impure else 'PASS'}")

    # ---- R1 FAST ROTATION -------------------------------------------------
    spin_snap = parse_last(spin_out, "SNAPSHOT")
    arc_snap = parse_last(arc_out, "SNAPSHOT")
    flag_snap = parse_last(flag_out, "SNAPSHOT")
    contract_source = ("presentation_snapshot_report -> "
                       "PresentationSnapshotStats")
    for label, snapshot in (("spin", spin_snap), ("spin-longarc", arc_snap),
                            ("spin-noflag", flag_snap)):
        problems = require_fields(snapshot, SNAPSHOT_STAT_CONTRACT,
                                  "[SNAPSHOT]", label, contract_source)
        if problems:
            for problem in problems:
                print(f"FAIL: {problem}", file=sys.stderr)
            return 1

    # Non-vacuity first, and in two independent directions. `rotarccheck` says
    # angles were reconstructed at all; `rotarcsnap` says this route actually
    # contains the past-quarter-turn population the snap clause grades. A route
    # that drifted off the spin sections would satisfy the first and not the
    # second, and R1 would then be a green row about nothing.
    if spin_snap["rotarccheck"] == 0:
        failures.append(
            "R1: rotarccheck=0 — no interpolated angle was audited on the spin "
            "route, so rotarcviolation=0 asserts nothing. The replay never "
            "resolved a rotation pair (presentation_snapshot.c "
            "rotation_arc_audit)")
    if spin_snap["rotarcsnap"] == 0:
        failures.append(
            "R1: rotarcsnap=0 — the spin route produced no rotation delta past "
            f"the quarter-turn snap over {spin_snap['rotarccheck']} audited "
            "angles, so the snap clause was never exercised. race_full_3lap on "
            "level 29 is the only route probed that produces this population "
            "(18 on the calibrating run); a route change that loses it must be "
            "replaced, not tolerated")
    if spin_snap["rotarcviolation"] != 0:
        failures.append(
            f"R1: rotarcviolation={spin_snap['rotarcviolation']} of "
            f"{spin_snap['rotarccheck']} audited interpolated angles landed "
            "outside the shortest arc between their two authored endpoints, or "
            "failed to snap past a quarter turn. An interpolated present lies "
            "BETWEEN its endpoints; this one takes the long way round, which is "
            "the smear artifact class C3 names. See "
            "docs/evidence/smoothing-artifact-repro-2026-08.md §1")
    if arc_snap["rotarcviolation"] == 0:
        failures.append(
            "R1 negative control: MDKR_TEST_ROTATION_LONG_ARC ran and produced "
            "ZERO arc violations. The control re-expresses every interpolated "
            "angle as the long way round the circle, so it must light this row "
            "up; a green control means the audit is not reading the pose the "
            "seam mutated, and R1's green above proves nothing")
    rows.append(
        f"[R1-FAST-ROTATION] checks={spin_snap['rotarccheck']} "
        f"snaps={spin_snap['rotarcsnap']} "
        f"violations={spin_snap['rotarcviolation']} "
        f"control={arc_snap['rotarcviolation']} "
        f"verdict={'PASS' if spin_snap['rotarcviolation'] == 0 and arc_snap['rotarcviolation'] > 0 and spin_snap['rotarcsnap'] > 0 else 'FAIL'}")

    # ---- R2 TELEPORT ------------------------------------------------------
    if spin_snap["disconthold"] == 0:
        failures.append(
            "R2: disconthold=0 — replay never met a spawn, respawn, warp or "
            "camera cut on the spin route, so discontblend=0 is a zero for "
            "want of an opportunity rather than for want of a defect. Three "
            "laps with collisions produce these; a route that stopped would "
            "have to be replaced")
    if spin_snap["discontblend"] != 0:
        failures.append(
            f"R2: discontblend={spin_snap['discontblend']} — replay blended "
            "across an entry published with the discontinuity flag set. That "
            "flag marks a pose change that is not motion (a spawn, a respawn, "
            "a warp, a camera cut), and blending it draws the object smeared "
            "along a line it never travelled")
    if flag_snap["discontblend"] == 0 or flag_snap["disconthold"] != 0:
        failures.append(
            "R2 negative control: MDKR_TEST_IGNORE_DISCONTINUITY ran and "
            f"reported discontblend={flag_snap['discontblend']}, "
            f"disconthold={flag_snap['disconthold']}. The control stops the "
            "flag being honoured, so every hold must become a blend; anything "
            "else means the seam did not reach the resolve path and R2's green "
            "above proves nothing")
    rows.append(
        f"[R2-TELEPORT] holds={spin_snap['disconthold']} "
        f"blends={spin_snap['discontblend']} "
        f"controlholds={flag_snap['disconthold']} "
        f"controlblends={flag_snap['discontblend']} "
        f"verdict={'PASS' if spin_snap['discontblend'] == 0 and spin_snap['disconthold'] > 0 and flag_snap['discontblend'] > 0 and flag_snap['disconthold'] == 0 else 'FAIL'}")

    # ---- R4 MONOTONIC ALPHA ----------------------------------------------
    #
    # The reverse-motion regression this pins is in the CHANGELOG's history: a
    # displayed present whose interpolation phase sat BEHIND its predecessor
    # drew the world walking backwards for one frame. The census differences
    # the monotone phase between consecutive DISPLAYED presents, which is the
    # correction §5.4 makes to an earlier reading -- a held present is never
    # displayed and contributes no sample, and its quarter tick of advance is
    # deferred into the following interval rather than lost.
    alpha_rows = alpha_delta_rows(spin_out)
    if not alpha_rows:
        print("FAIL: the spin arm emitted no [PRESENTPERF-HIST] "
              "series=alpha-delta row; MDKR_PRESENT_PERF plumbing is broken "
              "and R4 has no data", file=sys.stderr)
        return 1
    alpha = alpha_rows[-1]
    problems = require_fields(alpha, ALPHA_STAT_CONTRACT,
                              "[PRESENTPERF-HIST] series=alpha-delta", "spin",
                              "present_perf_pacing_summary -> present_sched.c")
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    sched = parse_last(spin_out, "PRESENTSCHED-SUMMARY")
    problems = require_fields(sched, SCHED_STAT_CONTRACT,
                              "[PRESENTSCHED-SUMMARY]", "spin",
                              "present_sched_summary -> present_sched.c")
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1

    if alpha["n"] == 0:
        failures.append(
            "R4: the alpha-delta census sampled no intervals, so every "
            "property below is vacuous")
    if alpha["regressions"] != 0:
        failures.append(
            f"R4: regressions={alpha['regressions']} — a displayed present "
            "carried an interpolation phase BEHIND its predecessor. That is "
            "the world walking backwards for one frame, and it is the exact "
            "regression the CHANGELOG records being fixed")
    if alpha["stalls"] != 0:
        failures.append(
            f"R4: stalls={alpha['stalls']} — two displayed presents landed on "
            "an identical phase, i.e. a frame the player cannot tell from its "
            "predecessor was put on the display. Distinct from a stale hold, "
            "which is never displayed and is outside this census by "
            "construction")
    if alpha["min"] < GRID_STEP_PPM:
        failures.append(
            f"R4: min={alpha['min']}ppm is below the {GRID_STEP_PPM}ppm grid "
            f"step. Two displayed presents cannot be closer together in phase "
            "than one interval of the alpha grid they are drawn on; a shorter "
            "interval means the phase was projected off the rational grid the "
            "subloop claims")
    # p50/p95/p99 are bin upper edges, so "at least 99% of intervals are
    # exactly the grid step" is p99 landing in the minimum's own bin.
    if alpha["p99"] > alpha["min"] + alpha["binwidth"]:
        failures.append(
            f"R4: p99={alpha['p99']}ppm is outside the minimum bin "
            f"({alpha['min']}..{alpha['min'] + alpha['binwidth']}ppm), so more "
            "than 1% of displayed intervals are longer than one grid step. "
            "The grid has become ragged")
    if alpha["max"] % GRID_STEP_PPM != 0:
        failures.append(
            f"R4: max={alpha['max']}ppm is not a whole multiple of the "
            f"{GRID_STEP_PPM}ppm grid step. The legal intervals above the "
            "minimum are exactly the deferred advance of one, two or more "
            "consecutive holds; a value between them is a phase error")
    if alpha["n"] != alpha["displayed"] - 1:
        failures.append(
            f"R4: the census reports n={alpha['n']} intervals over "
            f"displayed={alpha['displayed']} presents. Consecutive displayed "
            "presents produce exactly one fewer interval than presents; any "
            "other count means the series skipped or double-counted")
    if alpha["presents"] - alpha["displayed"] != sched["stale"]:
        failures.append(
            f"R4: presents-displayed={alpha['presents'] - alpha['displayed']} "
            f"but the scheduler reports stale={sched['stale']}. Those are the "
            "same quantity counted by two subsystems -- the presents that were "
            "held rather than shown -- and they have to agree before the "
            "deferred-advance arithmetic below means anything")
    # The deferred-advance identity. Every held present's quarter tick lands in
    # the following interval, so the census's total excess over the minimum is
    # exactly the holds' worth of advance. `mean` is integer-truncated, which
    # can hide up to n ppm of the sum -- hence the slack, which is four orders
    # of magnitude under one grid step.
    excess = (alpha["mean"] - alpha["min"]) * alpha["n"]
    deferred = sched["stale"] * GRID_STEP_PPM
    if abs(deferred - excess) > alpha["n"]:
        failures.append(
            f"R4: the displayed intervals carry {excess}ppm of advance above "
            f"the grid minimum, but {sched['stale']} held presents defer "
            f"{deferred}ppm. A hold's quarter tick is not lost, it lands in "
            "the following interval, so these must close to within the mean's "
            "own truncation")
    rows.append(
        f"[R4-MONOTONIC-ALPHA] n={alpha['n']} min={alpha['min']} "
        f"p99={alpha['p99']} max={alpha['max']} mean={alpha['mean']} "
        f"regressions={alpha['regressions']} stalls={alpha['stalls']} "
        f"presents={alpha['presents']} displayed={alpha['displayed']} "
        f"stale={sched['stale']} deferred={deferred} excess={excess}")

    # ---- R5 TICK-BOUNDARY STEP -------------------------------------------
    #
    # Steps are grouped into the tick they live inside before any mean is
    # taken, for two reasons. The comparison is WITHIN a tick -- one boundary
    # step against that tick's own three interiors -- so a tick contributing
    # only some of its steps would put a full step next to a mean taken over
    # fewer, which is not the same quantity. And it is what makes the hold
    # guard below able to drop a contaminated tick whole.
    #
    # THE HOLD GUARD. A held present is not a new image: the backend re-reads
    # the frame it already showed, so a hold lands in the dump as a step of
    # exactly zero. That biases R5's ratio in whichever direction it falls --
    # an interior hold shrinks the interior mean and inflates the excess, a
    # boundary hold shrinks the boundary mean and hides one. This route holds
    # 36 of 12,920 presents run-wide (0.28%), so a 120-frame window usually
    # contains none, and the diagnosis note's §5.1(c) checked exactly that by
    # hand ("none of the 119 steps in the sampled window is zero"). Usually is
    # not a property, so the check is made rather than assumed: any tick
    # carrying a zero step is dropped whole and counted, and the row prints the
    # count so a green verdict says which population it was taken over.
    #
    # The guard is reachable by construction and has been demonstrated: the
    # same arm at MDKR_PRESENT_SMOOTHING=off repeats each authored image three
    # times byte for byte at this rate (note §3, `a120-off`: 9,693 stale
    # presents), which drops every tick in the window and trips the floor
    # below. A scene static enough to produce a zero step without a hold would
    # also be dropped, which is the safe direction: R5 measures motion, and a
    # tick with no motion in it has nothing to say about the shape of motion.
    by_tick: dict[int, tuple[list[float], list[float]]] = {}
    for (previous_index, previous), (index, current) in zip(indexed, indexed[1:]):
        if index - previous_index != 1:
            continue
        step = mean_abs_step(previous, current)
        alpha_slot = index % PRESENTS_PER_TICK
        if alpha_slot == 0:
            # The step INTO an authored endpoint closes the tick that opened
            # PRESENTS_PER_TICK frames earlier.
            tick_start = index - PRESENTS_PER_TICK
            by_tick.setdefault(tick_start, ([], []))[0].append(step)
        else:
            tick_start = index - alpha_slot
            by_tick.setdefault(tick_start, ([], []))[1].append(step)

    complete = {start: steps for start, steps in by_tick.items()
                if len(steps[0]) == 1 and len(steps[1]) == PRESENTS_PER_TICK - 1}
    held = sorted(start for start, (boundary, interior) in complete.items()
                  if any(step == 0.0 for step in boundary + interior))
    for start in held:
        del complete[start]

    boundary_steps = [steps[0][0] for steps in complete.values()]
    interior_steps = [step for steps in complete.values() for step in steps[1]]
    if len(boundary_steps) < 8 or len(interior_steps) < 24:
        failures.append(
            f"R5: {len(boundary_steps)} complete ticks survive the window "
            f"({len(held)} dropped for containing a held present), which is "
            "too few to compare. A window this contaminated is not measuring "
            "the shape of motion; move the dump window or the route rather "
            "than reading the number it would produce")
        excess_ratio = 0.0
    else:
        boundary_mean = sum(boundary_steps) / len(boundary_steps)
        interior_mean = sum(interior_steps) / len(interior_steps)
        excess_ratio = (boundary_mean / interior_mean) - 1.0 if interior_mean \
            else math.inf
        if excess_ratio > BOUNDARY_EXCESS_MAX:
            failures.append(
                f"R5: the step into an authored endpoint is {boundary_mean:.3f} "
                f"against {interior_mean:.3f} inside the tick — "
                f"{excess_ratio * 100:+.2f}% against a {BOUNDARY_EXCESS_MAX * 100:.0f}% "
                "bound. On a uniform alpha grid, content every stage covers "
                "moves the same distance on all four steps, so a boundary "
                "excess is content advancing only at the authored tick. The "
                "fixed build measures +2.93% paired on this exact route and "
                "window and shipped 1.0.3 measured +16.68% pooled; this is a "
                "C1-class regression, not noise. See "
                "docs/evidence/smoothing-artifact-repro-2026-08.md §5.1")
        rows.append(
            f"[R5-TICK-BOUNDARY] boundary={boundary_mean:.3f} "
            f"interior={interior_mean:.3f} n={len(boundary_steps)}/"
            f"{len(interior_steps)} heldticks={len(held)} "
            f"excess={excess_ratio * 100:+.2f}% "
            f"bound={BOUNDARY_EXCESS_MAX * 100:.0f}% "
            f"verdict={'FAIL' if excess_ratio > BOUNDARY_EXCESS_MAX else 'PASS'}")

    rows.append(f"[BATTERY-RUNTIME] seconds={time.monotonic() - started:.0f}")
    for row in rows:
        print(row)
    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: the perceptual motion battery holds on every row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
