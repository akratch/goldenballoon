#!/usr/bin/env python3
"""check_boost_magnitude — the zip-pad boost is the magnitude DKR authored, and
it does not depend on how many racers are in the race.

    python3 tests/check_boost_magnitude.py [-v] [--build DIR] [--rom ROM]

Always muted + headless (MDKR_AUDIO=0 and --headless-frames), per tests/README.md.
Exit 0 = pass; exit 1 = at least one assertion failed (each printed with the
measured value).

WHAT THIS CLOSES (register: docs/open-items/gameplay.md, wave "zippad").
An eight-racer Tracks race was once measured at 44.9 world-units/frame off a zip
pad against 23.2 for "the same pad" in a solo Time Trial — 3.2x vs 1.67x top
speed — and the asymmetry was recorded as unverified rather than assumed correct.
This check is the closing measurement.

WHY IT ARMS THE BOOST INSTEAD OF DRIVING OVER A PAD.  The 44.9 figure came from a
pad that `nav_to_time_trial_race.txt` + `MDKR_AUTOPILOT` *happened* to drive over
in 2026-07.  It does not any more: the AI line moved when the wave "closedloop"
ROM-fidelity corrections landed, and the route now never touches SURFACE_ZIP_PAD
at all (measured: 0 frames of surface 3 over the whole race, and
`check_race_drive.py` reports max step 14.3 where it used to report 44.9).  That
is precisely the fixture class tests/README.md warns about — the racing *line* is
chaotic with respect to any simulation change — so no committed route can be
relied on to keep crossing one particular pad, and a check calibrated on one
would be measuring the AI, not the boost.

So `MDKR_ZIPPAD_BOOST=<frame>[:<ticks>]` (game/src/objects.c) arms the human
racer in exactly the state `racer.c:5727` arms it in for `SURFACE_ZIP_PAD` on a
car — `boostTimer = normalise_time(45)`, `boostType = BOOST_LARGE` — once, at a
fixed frame.  Everything downstream is untouched decomp code: all 310
boost/velocity statements in `game/src/racer.c` are byte-identical to the decomp
baseline in `.decomp-baseline`, so what is measured here IS the shipping boost,
with a deterministic trigger substituted for a chaotic one.

THE BROKEN DIRECTION (a check that cannot fail is not a check).  The `<ticks>`
field is the perturbed boost constant.  Two control arms run every time:

  * `:120` — holds the boost 2.7x as long. It reaches essentially the same peak,
    but the exact timer trace and post-45-tick tail reject it.
  * `:15`  — a third of the authored constant. The exact timer trace rejects it
    even if a favourable piece of road happens to preserve a similar peak.

Both must FAIL the baseline assertions.  If either one passes, this check fails
with POSITIVE CONTROL BROKEN, because that means the assertions below no longer
constrain the boost constant at all.

MEASURED (2026-08-03, arming frame 4000, cadence enhanced / 1 field, Ancient Lake,
default car, `MDKR_AUTOPILOT`; every number reproduced run to run):

  arm                       cruise   boost frames   peak |velocity|   peak/cruise
  8-racer Tracks            12.542        45           22.241            1.97x
  solo Time Trial           12.769        45           22.323            1.95x
  control :15               12.571        15           21.667            1.90x
  control :120              12.494       120           22.361            1.98x

The eight-racer and the solo Time Trial peaks differ by 0.082 velocity units —
0.37% — with the boost armed identically.  There is no racer-count coupling in
the mechanism, and `normalise_time()` (objects.c:1341) has no framerate term
either: it is a PAL 5/6 rescale of the constant and nothing else.

The `:120` row is the physically interesting one and is why peak velocity alone
is not enough: a 2.7x longer boost reaches nearly the same peak. The authoritative
evidence for the constant is the consecutive 45..1 timer sequence followed by
zero, with BOOST_LARGE retained throughout. Velocity still proves the magnitude
and racer-count independence, but it is not required to be monotonic: steering,
road gradient, walls, and AI braking remain live downstream of the boost.

VERDICT RECORDED: authored, not a port defect.  The historical 44.9-vs-23.2 pair
was never a controlled comparison — two different racing lines, at two different
points of two different tracks-modes, with two different entry speeds — and
neither figure is reproducible on current source.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary

RACE_SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"   # 8-racer Tracks
TT_SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"          # solo Time Trial

ARM_FRAME = 4000       # comfortably past the countdown (race start ~3120) and at
                       # steady cruise in both fixtures
FRAMES = 4200          # ARM_FRAME + the 90-frame observation window + slack
SPAN = 90              # observation window, in frames, from the arming frame
STRIDE = 3             # trace sample stride -> 30 samples per arm

BOOST_TICKS = 45       # the authored constant: racer.c:5727 normalise_time(45)

# --- thresholds (provenance: the measured table in the module docstring) -------
#
# THE TRACE IS ASSERTED ON |racer->velocity|, NOT ON THE POSITION STEP.  Both are
# available and the position step is what the other motion checks use, but the
# step includes cornering and gradient, so it carries the racing line into the
# measurement: normalised by cruise, the two fixtures' ramps differ by up to 0.17
# where the tolerance would have to be 0.25.  The velocity trace does not -- the
# two fixtures agree to 0.05 across the whole plateau, which is what makes a
# tight envelope safe here.  The step is still used for the cruise reference and
# reported, because "world units per frame" is the unit the register is in.
#
# Entry speed at the arming frame, i.e. the racer is cruising, not stopped or
# already boosting.  Measured 12.128 (race) / 12.608 (time trial).
ENTRY_VEL_MIN, ENTRY_VEL_MAX = 9.0, 16.0
# Cruise sanity: the median non-boost in-race position step.  Measured 12.23..12.77.
CRUISE_MIN, CRUISE_MAX = 10.0, 16.0
# Peak |velocity| in the window: the boost's terminal speed.  Measured 22.357
# (race) and 22.336 (time trial) -- and 22.358 when the timer is held 2.7x
# longer, which is the saturation result the docstring describes.
PEAK_VEL_MIN, PEAK_VEL_MAX = 21.5, 23.0
#
# TAIL -- samples 18..29 (frames +55 .. +88), well after a 45-tick boost has
# decayed, as a fraction of the run's own plateau.  Measured 0.556 on the race
# fixture and 0.337 solo (the AI is braking into a corner there); the :120
# control sits at 0.960.  This is the assertion that catches a boost constant
# perturbed UPWARD, which the peak cannot see because the boost saturates.
TAIL_FROM, TAIL_TO = 18, 30
TAIL_FRACTION_MAX = 0.75
#
# Cross-mode: the G1 question itself.  Measured difference 0.021 velocity units.
CROSS_MODE_VEL_TOL = 0.5

BOOST_RE = re.compile(
    r"\[BOOST\] frame=(\d+) timer=(-?\d+) type=(-?\d+) vel=(\S+) "
    r"x=(\S+) y=(\S+) z=(\S+) surf=(-?\d+) grounded=(-?\d+) start=(-?\d+)")
ARM_RE = re.compile(r"\[BOOSTARM\] frame=(\d+) ticks=(-?\d+) timer=(-?\d+)")
SANITIZER_RE = re.compile(
    r"AddressSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer|"
    r"runtime error:|SUMMARY: .*Sanitizer")


class Arm:
    """One headless run, reduced to the numbers the assertions need."""

    def __init__(self, name: str, script: str, ticks: int | None):
        self.name = name
        self.script = script
        self.ticks = ticks
        self.errors: list[str] = []
        self.cruise = 0.0
        self.peak_vel = 0.0
        self.peak_step = 0.0
        self.peak_frame = 0
        self.boost_frames = 0
        self.entry_vel = 0.0
        self.vel: list[float] = []       # |velocity| sampled every STRIDE frames
        self.timer_trace: list[int] = []
        self.type_trace: list[int] = []
        self.profile: list[float] = []   # position step / cruise, same sampling


def run_arm(binary: str, rom: str, arm: Arm, verbose: bool) -> Arm:
    """Launch one arm and reduce its [BOOST] stream. Never raises."""
    spec = str(ARM_FRAME) if arm.ticks is None else f"{ARM_FRAME}:{arm.ticks}"
    # A fresh save dir per arm: tests/README.md keeps the suite sequential
    # because fixtures replace save/eeprom.bin, and nothing here wants a
    # record written at all.
    save_dir = tempfile.mkdtemp(prefix="mdkr_boost_")
    env = dict(os.environ,
               MDKR_AUDIO="0",              # belt-and-braces; --headless is the guarantee
               MDKR_SIMULATION_CADENCE="enhanced",
               MDKR_SYNTH_FIELDS="1",       # updateRate pinned: the trace is per
                                            # simulated field, not per host frame
               MDKR_AUTOPILOT="1",          # closed loop: DKR's own AI drives
               MDKR_TRACE="1",
               MDKR_BOOST_TRACE="1",
               MDKR_ZIPPAD_BOOST=spec,
               MDKR_SAVE_DIR=save_dir)
    # Nothing inherited may perturb the physics under measurement.
    for stale in ("MDKR_FORCE_BOOST", "MDKR_BOSS_SLOW", "MDKR_FORCE_LAPS",
                  "MDKR_RNGSEED", "MDKR_ARCTAN", "MDKR_TRIG", "MDKR_LOAD_TRACK",
                  "MDKR_DRIVE_ROUTE", "MDKR_PACE_REALTIME", "MDKR_VI_PACE"):
        env.pop(stale, None)
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", arm.script, "--rom", rom]
    if verbose:
        print(f"$ MDKR_ZIPPAD_BOOST={spec} " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr

    if proc.returncode != 0:
        arm.errors.append(f"exit code {proc.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            arm.errors.append(f"{marker} in output: {line.strip()}")
    m = SANITIZER_RE.search(out)
    if m is not None:
        line = next((l for l in out.splitlines() if SANITIZER_RE.search(l)),
                    m.group(0))
        arm.errors.append(f"sanitizer diagnostic: {line.strip()}")

    if not ARM_RE.search(out):
        arm.errors.append(f"the boost was never armed — no [BOOSTARM] at frame "
                          f"{ARM_FRAME}; the run did not reach the race")
        return arm

    rows = []
    for line in out.splitlines():
        b = BOOST_RE.search(line)
        if b:
            rows.append(dict(f=int(b.group(1)), t=int(b.group(2)),
                             type=int(b.group(3)),
                             vel=float(b.group(4)), x=float(b.group(5)),
                             y=float(b.group(6)), z=float(b.group(7)),
                             start=int(b.group(10))))
    if len(rows) < SPAN:
        arm.errors.append(f"only {len(rows)} [BOOST] rows traced (want >= {SPAN})")
        return arm

    step = {}
    for a, b in zip(rows, rows[1:]):
        if b["f"] != a["f"] + 1:
            continue
        d = math.dist((a["x"], a["y"], a["z"]), (b["x"], b["y"], b["z"]))
        if d == d and d < 1e30:          # reject nan/inf rather than propagate it
            step[b["f"]] = d

    # Cruise = the median step over in-race, non-boosting frames.  Median, not
    # mean, so a corner or a wall cannot move the normalisation.
    quiet = sorted(step[r["f"]] for r in rows
                   if r["start"] == 0 and r["t"] == 0 and r["f"] in step
                   and not (ARM_FRAME <= r["f"] <= ARM_FRAME + SPAN))
    if not quiet:
        arm.errors.append("no non-boost in-race frames to normalise against")
        return arm
    arm.cruise = quiet[len(quiet) // 2]

    window = [r for r in rows if ARM_FRAME <= r["f"] <= ARM_FRAME + SPAN]
    by_frame = {r["f"]: abs(r["vel"]) for r in rows}
    arm.boost_frames = sum(1 for r in rows if r["t"] != 0)
    arm.entry_vel = by_frame.get(ARM_FRAME, 0.0)
    arm.peak_vel = max(abs(r["vel"]) for r in window)
    arm.peak_step, arm.peak_frame = max(
        ((step.get(r["f"], 0.0), r["f"]) for r in window), default=(0.0, 0))
    timer_window = {
        r["f"]: (r["t"], r["type"])
        for r in rows
        if ARM_FRAME <= r["f"] <= ARM_FRAME + BOOST_TICKS
    }
    arm.timer_trace = [
        timer_window.get(frame, (-1, -1))[0]
        for frame in range(ARM_FRAME, ARM_FRAME + BOOST_TICKS + 1)
    ]
    arm.type_trace = [
        timer_window.get(frame, (-1, -1))[1]
        for frame in range(ARM_FRAME, ARM_FRAME + BOOST_TICKS + 1)
    ]
    samples = range(ARM_FRAME + 1, ARM_FRAME + SPAN + 1, STRIDE)
    arm.vel = [by_frame.get(f, 0.0) for f in samples]
    arm.profile = [step.get(f, 0.0) / arm.cruise for f in samples]
    return arm


def assess(arm: Arm) -> list[str]:
    """The baseline assertions. Returns the list of violations (empty == pass)."""
    bad = list(arm.errors)
    if bad:
        return bad
    # 1. the boost lasts the authored number of ticks, with no skipped,
    # duplicated, or presentation-rate-dependent timer state.
    if arm.boost_frames != BOOST_TICKS:
        bad.append(f"boost lasted {arm.boost_frames} frames, want exactly "
                   f"{BOOST_TICKS} (normalise_time(45) at updateRate 1)")
    expected_timer = list(range(BOOST_TICKS, -1, -1))
    if arm.timer_trace != expected_timer:
        mismatch = next(
            (index for index, (actual, expected) in enumerate(
                zip(arm.timer_trace, expected_timer)) if actual != expected),
            min(len(arm.timer_trace), len(expected_timer)),
        )
        actual = arm.timer_trace[mismatch] if mismatch < len(arm.timer_trace) else None
        expected = expected_timer[mismatch] if mismatch < len(expected_timer) else None
        bad.append(
            f"boost timer sequence diverged at frame +{mismatch}: "
            f"got {actual}, expected {expected} in the exact 45..0 countdown"
        )
    active_types = arm.type_trace[:BOOST_TICKS]
    if active_types != [2] * BOOST_TICKS:
        bad.append("boost type did not remain BOOST_LARGE for all 45 active ticks")

    # 2. the run was cruising when the boost was armed
    if not CRUISE_MIN <= arm.cruise <= CRUISE_MAX:
        bad.append(f"cruise speed {arm.cruise:.3f} outside "
                   f"[{CRUISE_MIN}, {CRUISE_MAX}] units/frame")
    if not ENTRY_VEL_MIN <= arm.entry_vel <= ENTRY_VEL_MAX:
        bad.append(f"|velocity| at the arming frame is {arm.entry_vel:.3f}, "
                   f"outside [{ENTRY_VEL_MIN}, {ENTRY_VEL_MAX}] — the racer was "
                   f"not cruising, so the boost is not being measured from a "
                   f"comparable state")

    # 3. terminal speed
    if not PEAK_VEL_MIN <= arm.peak_vel <= PEAK_VEL_MAX:
        bad.append(f"peak |velocity| {arm.peak_vel:.3f} outside "
                   f"[{PEAK_VEL_MIN}, {PEAK_VEL_MAX}]")

    # 4. Post-boost decay catches an overlong constant without pretending the
    # live road/steering path must produce a monotonic velocity curve.
    tail = arm.vel[TAIL_FROM:TAIL_TO]
    if tail and arm.peak_vel:
        frac = (sum(tail) / len(tail)) / arm.peak_vel
        if frac > TAIL_FRACTION_MAX:
            bad.append(f"still boosting after the boost: mean |velocity| over "
                       f"frames +{1 + TAIL_FROM * STRIDE}..+"
                       f"{1 + (TAIL_TO - 1) * STRIDE} is {frac:.3f} of the "
                       f"plateau (limit {TAIL_FRACTION_MAX}) — a "
                       f"{BOOST_TICKS}-tick boost has fully decayed by then")
    return bad


def describe(arm: Arm, verbose: bool) -> None:
    if not verbose:
        return
    if not arm.cruise:
        print(f"  [{arm.name}] no usable trace")
        return
    print(f"  [{arm.name}] cruise={arm.cruise:.3f} entryVel={arm.entry_vel:.3f} "
          f"boostFrames={arm.boost_frames} peakVel={arm.peak_vel:.3f} "
          f"peakStep={arm.peak_step:.3f} (={arm.peak_step / arm.cruise:.2f}x cruise) "
          f"at frame {arm.peak_frame}")
    if arm.vel:
        print("    |velocity| early:        "
              + " ".join(f"{v:.2f}" for v in arm.vel[:12]))
        print("    |velocity| tail:         "
              + " ".join(f"{v:.2f}" for v in arm.vel[TAIL_FROM:TAIL_TO]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, RACE_SCRIPT, TT_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []

    # ---- baseline arms: the authored constant, in both racer counts -----------
    race = run_arm(binary, args.rom,
                   Arm("8-racer Tracks", RACE_SCRIPT, None), args.verbose)
    describe(race, args.verbose)
    failures += [f"[8-racer Tracks] {e}" for e in assess(race)]

    trial = run_arm(binary, args.rom,
                    Arm("solo Time Trial", TT_SCRIPT, None), args.verbose)
    describe(trial, args.verbose)
    failures += [f"[solo Time Trial] {e}" for e in assess(trial)]

    # ---- the G1 question: is the magnitude racer-count dependent? ------------
    if race.peak_vel and trial.peak_vel:
        delta = abs(race.peak_vel - trial.peak_vel)
        if args.verbose:
            print(f"  cross-mode peak |velocity| difference: {delta:.4f}")
        if delta > CROSS_MODE_VEL_TOL:
            failures.append(
                f"the zip-pad boost is racer-count dependent: peak |velocity| "
                f"{race.peak_vel:.3f} with eight racers vs {trial.peak_vel:.3f} "
                f"solo, difference {delta:.3f} (limit {CROSS_MODE_VEL_TOL}). "
                f"The mechanism is DKR's own and must not know how many racers "
                f"are in the race — see docs/open-items/gameplay.md wave "
                f"\"zippad\".")

    # ---- broken direction: perturbed boost constants must FAIL ---------------
    for ticks in (120, 15):
        ctl = run_arm(binary, args.rom,
                      Arm(f"control :{ticks}", RACE_SCRIPT, ticks), args.verbose)
        describe(ctl, args.verbose)
        violations = assess(ctl)
        if args.verbose:
            print(f"    control :{ticks} tripped {len(violations)} assertion(s): "
                  + ("; ".join(v[:70] for v in violations) or "NONE"))
        if not violations:
            failures.append(
                f"POSITIVE CONTROL BROKEN: boostTimer perturbed 45 -> {ticks} and "
                f"every assertion still passed, so this check does not constrain "
                f"the boost constant.")

    if args.verbose or failures:
        for arm in (race, trial):
            if arm.cruise:
                print(f"  {arm.name}: peak |velocity| {arm.peak_vel:.3f}, "
                      f"{arm.peak_step / arm.cruise:.2f}x cruise, "
                      f"{arm.boost_frames} boost frames")

    if failures:
        print("check_boost_magnitude: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_boost_magnitude: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
