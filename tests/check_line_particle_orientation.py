#!/usr/bin/env python3
"""check_line_particle_orientation — a line particle's ribbon spreads along the
axis the ROM authored, so the plane's wing contrails lie flat instead of on edge.

    python3 tests/check_line_particle_orientation.py [-v] [--build DIR] [--rom ROM]

Always muted + headless (MDKR_AUDIO=0 and --headless-frames), per tests/README.md.
Exit 0 = pass; exit 1 = at least one assertion failed (each printed with the
measured value).

WHAT THIS CLOSES (issue #24: "plane wing contrails render rotated 90 degrees").

A line particle is a ribbon: update_line_particle() (game/src/particles.c) takes
a half-width offset along ONE of the parent's local axes, rotates it by the
parent's rotation, and lays the trail's vertices out either side of the emitter
path. Which local axis it picks is `lineOrientation`, and DKR authors that per
particle descriptor: 0 -> local X, 1 -> local Y, 2 -> local Z. A plane's wing
contrail is authored 0 -- the ribbon spreads across the wings, so the trail is a
flat horizontal band ("--"). Orientation 1 turns the same ribbon on its side and
the trail becomes a vertical sheet, drawn edge-on from a chase camera as a
hairline ("|").

The port read orientation 1 for every line particle in the game. `lineOrientation`
is a 6-bit C bitfield inside the descriptor's 0x0A halfword, and the ROM authored
that halfword for the N64's big-endian compiler, which packs bitfields from the
MOST significant bit down. A little-endian host packs from the least significant
bit up, so the identical declaration reads bits 5..0 instead of bits 15..10. The
asset swapper corrects the halfword's numeric VALUE, which cannot help: the
disagreement is about bit position inside that value. Stock line descriptors all
store 0x0001 -- orientation 0 with a 1 left in the four padding bits -- so the
host read the padding and got orientation 1. See the PARTICLE_DESC_* accessors in
game/src/particles.h for the full derivation.

MEASURED (2026-08-07, Hot Top Volcano / plane, MDKR_AUTOPILOT, 3000 frames,
remastered, GL; identical run to run). Both plane trail descriptors, every one of
the 2052 emitted ribbons:

    build              desc 37    desc 35    local offset      trail
    before the fix     orient 1   orient 1   (0, 2, 0)         vertical sheet
    after the fix      orient 0   orient 0   (2, 0, 0)         flat band

and the dumped frame at 2800 shows the difference directly: hairline streaks
behind each wingtip before, broad soft bands after.

WHY THE LOCAL OFFSET AND NOT THE ORIENTATION NUMBER. The trace carries both, and
this check asserts the local offset vector -- the geometry the ribbon is actually
built from -- rather than trusting the enum that selected it. An orientation
value that stopped reaching the vertex maths would still read 0 and pass a
number-only assertion; a ribbon that stopped being flat could not. The WORLD
vector is reported for context but never asserted: it is the local vector after
the plane's own pitch/roll, so it moves with the racing line and says nothing
about orientation on its own. That roll-independence is the point -- it is what
makes the assertion survive the bank the issue describes ("most visible holding
R").

THE BROKEN DIRECTION (a check that cannot fail is not a check). Two guards:

  * POSITIVE CONTROL, in process: the same assertion is run against a synthetic
    row carrying the pre-fix geometry measured above (orient 1, local (0, 2, 0)).
    It MUST be rejected. If it passes, the assertion no longer constrains the
    ribbon axis and this check fails with POSITIVE CONTROL BROKEN.
  * NON-VACUITY: the run must produce at least MIN_ROWS ribbons from at least
    MIN_DESCRIPTORS distinct descriptors. A route that stopped emitting line
    particles would otherwise "pass" with nothing measured.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary

# Hot Top Volcano (level 7) authors plane as its DEFAULT vehicle
# (check_vehicle_sweep.py --matrix: "level 7 default=plane avail=0x6"), so the
# racer is airborne for the whole route and both wing emitters run continuously.
LEVEL = 7
VEHICLE = 2  # VEHICLE_PLANE
SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
FRAMES = 1200  # first ribbon measured at frame 90; 1200 leaves a wide margin

# Non-vacuity floors. Measured 2052 rows / 2 descriptors over 3000 frames, and
# 862 rows / 1 descriptor over this 1200-frame budget (the second descriptor only
# starts emitting later in the lap). Floors sit well under both.
MIN_ROWS = 100
MIN_DESCRIPTORS = 1

# Which local axis each authored orientation names, per update_line_particle().
ORIENTATION_AXIS = {0: "x", 1: "y", 2: "z"}
# The plane's wing contrail is authored flat across the wings.
EXPECTED_AXIS = "x"

# The ribbon half-width is a real distance, not a rounding artefact: measured
# 2.0 and 2.42 world units for the two descriptors. A ribbon whose "long" axis
# collapsed toward zero would be no ribbon at all.
MIN_HALF_WIDTH = 0.1
# The two axes the ribbon does NOT spread along are written as literal 0.0f by
# update_line_particle() before the rotation, so they are exactly zero. Allow a
# hair of slack rather than compare floats for equality.
MAX_OFF_AXIS = 1e-4

ROW_RE = re.compile(
    r"\[LINEPART\] frame=(\d+) desc=(-?\d+) orient=(-?\d+) "
    r"local=(\S+?),(\S+?),(\S+?) world=(\S+?),(\S+?),(\S+)")


class Row:
    """One emitted ribbon, reduced to what the assertions need."""

    def __init__(self, frame: int, desc: int, orient: int,
                 local: tuple[float, float, float],
                 world: tuple[float, float, float]):
        self.frame = frame
        self.desc = desc
        self.orient = orient
        self.local = local
        self.world = world

    def __str__(self) -> str:
        return ("frame=%d desc=%d orient=%d local=(%g, %g, %g)"
                % (self.frame, self.desc, self.orient,
                   self.local[0], self.local[1], self.local[2]))


def dominant_axis(local: tuple[float, float, float]) -> str | None:
    """The single local axis this ribbon spreads along, or None if it is not
    axis-aligned (two or more components carry length)."""
    named = tuple(zip("xyz", (abs(v) for v in local)))
    carrying = [name for name, mag in named if mag > MAX_OFF_AXIS]
    if len(carrying) != 1:
        return None
    axis = carrying[0]
    if dict(named)[axis] < MIN_HALF_WIDTH:
        return None
    return axis


def check_row(row: Row) -> str | None:
    """Reject a ribbon that is not the flat, wing-aligned band DKR authored.
    Returns a failure string, or None if the ribbon is correct."""
    axis = dominant_axis(row.local)
    if axis is None:
        return ("ribbon is not axis-aligned or has no width: %s" % row)
    if axis != EXPECTED_AXIS:
        return ("ribbon spreads along local %s, authored local %s -- the trail "
                "is turned out of the flat band the console draws: %s"
                % (axis, EXPECTED_AXIS, row))
    # The geometry and the enum that selected it must agree; a mismatch means
    # the trace no longer describes the vertex maths it is standing in for.
    if ORIENTATION_AXIS.get(row.orient) != axis:
        return ("orientation %d names local %s but the ribbon was built on "
                "local %s: %s"
                % (row.orient, ORIENTATION_AXIS.get(row.orient, "?"), axis, row))
    return None


def positive_control() -> str | None:
    """The pre-fix geometry, measured. check_row() MUST reject it."""
    unfixed = Row(90, 37, 1, (0.0, 2.0, 0.0), (-0.919724, 0.814042, 1.57817))
    if check_row(unfixed) is None:
        return ("POSITIVE CONTROL BROKEN: check_row() accepted the measured "
                "pre-fix ribbon (%s); the assertion no longer constrains the "
                "ribbon axis" % unfixed)
    return None


def run(binary: str, rom: str, verbose: bool) -> tuple[list[Row], list[str]]:
    """Drive the plane route once and reduce its [LINEPART] stream."""
    errors: list[str] = []
    # A fresh save dir per run: tests/README.md keeps the suite sequential
    # because fixtures replace save/eeprom.bin, and nothing here wants a record
    # written at all.
    save_dir = tempfile.mkdtemp(prefix="mdkr_linepart_")
    env = dict(os.environ,
               MDKR_AUDIO="0",  # belt-and-braces; --headless-frames is the guarantee
               MDKR_AUTOPILOT="1",  # closed loop: DKR's own AI flies the plane
               MDKR_TRACE="1",
               MDKR_LINE_PARTICLE_TRACE="1",
               MDKR_LOAD_TRACK="%d:%d" % (LEVEL, VEHICLE),
               MDKR64_HIDDEN="1",
               MDKR_SAVE_DIR=save_dir,
               LC_ALL="C")
    cmd = [binary,
           "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT,
           "--rom", rom,
           "--window-size", "320x240"]
    if verbose:
        print("  $ MDKR_LOAD_TRACK=%s %s" % (env["MDKR_LOAD_TRACK"], " ".join(cmd)))
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                          timeout=900)
    output = proc.stdout + proc.stderr
    if proc.returncode != 0:
        errors.append("run exited %d" % proc.returncode)
    fatal = find_fatal(output)
    if fatal:
        errors.append("fatal in run output: %s" % fatal)

    rows = []
    for m in ROW_RE.finditer(output):
        rows.append(Row(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                        (float(m.group(4)), float(m.group(5)), float(m.group(6))),
                        (float(m.group(7)), float(m.group(8)), float(m.group(9)))))
    return rows, errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    if not os.path.exists(args.rom):
        print("FAIL rom not found: %s" % args.rom)
        return 1

    failures: list[str] = []

    control = positive_control()
    if control:
        print("FAIL %s" % control)
        return 1
    print("ok   positive control: the measured pre-fix ribbon is rejected")

    rows, errors = run(binary, args.rom, args.verbose)
    failures.extend(errors)

    descriptors = sorted({r.desc for r in rows})
    if len(rows) < MIN_ROWS:
        failures.append("VACUOUS: %d line-particle ribbons emitted, need >= %d "
                        "(the route stopped producing trails)"
                        % (len(rows), MIN_ROWS))
    if len(descriptors) < MIN_DESCRIPTORS:
        failures.append("VACUOUS: %d distinct line descriptors, need >= %d"
                        % (len(descriptors), MIN_DESCRIPTORS))
    if len(rows) >= MIN_ROWS:
        print("ok   non-vacuity: %d ribbons from descriptor(s) %s"
              % (len(rows), ", ".join(str(d) for d in descriptors)))

    bad: list[str] = []
    for row in rows:
        why = check_row(row)
        if why:
            bad.append(why)
    if bad:
        failures.append("%d of %d ribbons are not the authored flat band; "
                        "first: %s" % (len(bad), len(rows), bad[0]))
    elif rows:
        sample = rows[0]
        print("ok   every ribbon spreads along local %s "
              "(e.g. %s, world=(%g, %g, %g))"
              % (EXPECTED_AXIS, sample, sample.world[0], sample.world[1],
                 sample.world[2]))

    for line in failures:
        print("FAIL %s" % line)
    if failures:
        return 1
    print("PASS check_line_particle_orientation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
