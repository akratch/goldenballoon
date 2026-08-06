#!/usr/bin/env python3
"""Automated in-race drive check: catches SILENT gameplay breakage.

Why this exists
---------------
Every other fixture in tests/ passes if the process exits 0 with no [CRASH] /
[FATAL].  The whole class of bugs where the *player racer* ends up somewhere it
should never be produces neither: the racer is flung out of the world, the
segment lookup stops matching, traverse_segments_bsp_tree() returns no visible
segments, and the screen becomes a flat fog-brown field with only the HUD and
minimap on it — while the race clock keeps counting and the run still exits 0.
That is how the ASSET_MISC_8 denormal-divisor bug survived a full validation
matrix (see docs/OPEN_ITEMS.md).

So this check asserts on what actually matters:

  1. POSITION SANITY   — no inf/nan, y inside a generous track band, and no
                         TELEPORT.  A teleport is identified by its *shape*, not
                         by an absolute speed: see MAX_ACCEL below.
  2. FORWARD PROGRESS  — the traced `cp=` (courseCheckpoint: checkpoints crossed
                         since the race start, never reset per lap) must climb to
                         a real count, `lap=` must reach at least 1, and the kart
                         must never sit still for a long stretch.  Position alone
                         cannot tell "driving the track" from "ping-ponging
                         between a respawn point and a wall", which is exactly
                         what the broken build did.
  3. SCENE STILL DRAWS — sampled frames (including the last) must contain a real
                         rendered scene, measured on a centre crop that excludes
                         the HUD band and the minimap corner.  Measured:
                         good frames 620..3400 distinct colours / luma sigma
                         21..48; the "world stopped drawing" frame scores 59 and
                         5.9.  Thresholds sit far from both.

CLOSED-LOOP, since the "closedloop" wave
----------------------------------------
This check used to replay `race_drive_long.txt` — throttle held, stick pulsed
LEFT for 25 frames every 120 — and assert against that one trajectory.  That is
the fragile-fixture class: a *blind open-loop route* whose line is chaotic with
respect to any change in the simulation, so every physics or RNG change
re-strands it and the check fails for a reason unrelated to what it tests.  It
had already been recalibrated once for that reason (`MIN_FINAL_CP` 20 -> 15 in
P3.5), and it failed again the moment the ROM's RNG seed / arctan table /
table-based trig became the defaults: cp 14 instead of 15, lap 0 instead of 1,
and a 240-frame window averaging 0.11 units/frame because the kart had nosed into
a wall.  Nothing about the port was broken.

It now drives with `MDKR_AUTOPILOT=1` — `racer_AI_pathing_inputs()`, the driver
every CPU racer uses and the one that laps real tracks in the attract demo — over
`nav_to_time_trial_race.txt`, which is exactly `race_drive_long.txt`'s navigation
with the (now unnecessary) driving inputs removed.  The AI steers toward the next
AI node every frame, so it *corrects* after a perturbation instead of
accumulating it, and the outcome asserted below is reached rather than replayed.

What that costs and what it does not: the autopilot overrides `gCurrentStickX` /
`gCurrentRacerInput` **after** update_player_racer's normal input dispatch, so
every line of physics downstream — including `func_80050A28()`, the lateral-
velocity degrade where the ASSET_MISC_8 divisor bug fired — runs identically.
The human input path itself is still exercised by this route's menu navigation and
by check_race_2p_split.

VERIFIED IN THE BROKEN DIRECTION.  `ASSET_MISC_8` was removed from
`dkr_misc_normalize_tables()`'s word-array list (game/src/objects.c), i.e. the
denormal-divisor bug was deliberately reintroduced, and this check fails on six
independent assertions at once:

    - exit code -6
    - [FATAL] update_player_racer: non-finite position (nan, inf, nan) after
      vehicle 0 update — velocity=(-inf, inf, -inf) lateral=-inf
    - only 14 in-race frames traced (want >= 1000)
    - only reached checkpoint 0 (want >= 30)
    - only reached lap 0 (want >= 2)
    - only 1 frames dumped (want >= 4)

The autopilot reaches it *sooner* than the old open-loop route did, because the AI
steers on the first frame out of the grid instead of waiting for a scripted pulse
at frame 3000.

Usage:
    tests/check_race_drive.py [--build build] [--rom baserom.us.v80.z64]
                              [--keep-frames DIR] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary

SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"
FRAMES = 7500          # ~4380 in-race frames; the AI finishes 3 laps at ~8100
RACE_START = 3120      # countdown clears here (the clock starts counting; 3118)

# --- thresholds (see the module docstring for the measurements behind them) ---
#
# TELEPORT DETECTION IS ON SHAPE, NOT ON SPEED.  The old absolute 40.0
# units/frame cap does not survive a route that actually takes a zip pad in an
# eight-racer Tracks race: measured 44.9 units/frame sustained for 55 frames,
# on grass, with all four wheels reporting a surface, ramping smoothly up from 13
# and back down — i.e. `boostTimer = normalise_time(45)` / `BOOST_LARGE` from
# `lastWheelSurface == SURFACE_ZIP_PAD` (racer.c:1945), doing exactly what a boost
# should.  A solo Time Trial peaks at 23.2 over 41 frames on the same pad, so the
# extra is the eight-racer race's own banana/boost stacking, not a defect.
#
# The ASSET_MISC_8 signature is the opposite shape: one frame of 1296.8 units with
# normal speed either side, because lateral_velocity went to -inf and back.  So
# what is asserted is the per-frame CHANGE in step length (MAX_ACCEL) plus a very
# generous absolute ceiling.  Measured on a healthy run: max acceleration 5.8
# units/frame^2 (frame 3916, inside the boost), max step 44.9.  Neither is
# calibrated on a particular racing line.
#
# NOTE the 44.9 is 3.2x this car's top speed on this track, against 1.67x for the
# same zip pad in a solo Time Trial.  The mechanism is the game's own and is not
# touched by anything in the "closedloop" wave, but the magnitude is unverified
# against the ROM -- recorded in docs/OPEN_ITEMS.md rather than assumed correct,
# which is also why this check no longer depends on the answer.
MAX_STEP = 150.0       # absolute ceiling; observed 44.9, historic break 1296.8
MAX_ACCEL = 40.0       # change in step length between consecutive frames;
                       # observed 5.8 healthy, ~1284 on the historic break
Y_MIN, Y_MAX = -150.0, 450.0    # Ancient Lake: observed 13 .. 269
# Measured with the autopilot: cp 49, lap 2 by frame 7500 (the AI crosses ~6.5
# checkpoints per 500 frames and completes lap 1 at clock ~1750).  The old
# open-loop route managed cp 19 / lap 1 on a good day and cp 14 / lap 0 after any
# perturbation; a deliberately-broken build scores 0.  These are set well below
# the measurement because the AI line is stable but not sacred — what they have to
# separate is "drove multiple laps of the track" from "went nowhere".
MIN_FINAL_CP = 30
MIN_FINAL_LAP = 2
STALL_WINDOW = 240     # frames
STALL_MIN_MEAN_SPEED = 1.5      # units/frame averaged over STALL_WINDOW
                                # (healthy slowest window 10.35; the failing
                                # open-loop route scored 0.11)
MIN_DISTINCT_COLOURS = 200      # broken: 59   good: >= 620
MIN_LUMA_SIGMA = 12.0           # broken: 5.9  good: >= 21.6
SAMPLE_EVERY = 800     # frame-dump stride

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)")
SANITIZER_RE = re.compile(
    r"AddressSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer|"
    r"runtime error:|SUMMARY: .*Sanitizer"
)


def parse_float(tok: str) -> float:
    """float() accepts 'inf'/'nan', which is what we want to detect, not reject."""
    return float(tok)


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def scene_metrics(path: str) -> tuple[int, float]:
    """(distinct 5-bit-quantized colours, luma std-dev) over a centre crop.

    The crop drops the top 35% (HUD: place / lap / item / timer) and the right
    edge is kept because the minimap is small relative to the crop; what matters
    is that a flat field scores near zero on both metrics while any real scene
    (track + sky + kart + scenery) scores an order of magnitude higher.
    """
    w, h, px = read_ppm(path)
    x0, x1 = int(w * 0.15), int(w * 0.85)
    y0, y1 = int(h * 0.35), int(h * 0.97)
    colours = set()
    n = total = total_sq = 0
    for y in range(y0, y1, 3):
        row = y * w * 3
        for x in range(x0, x1, 3):
            o = row + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            colours.add((r >> 3, g >> 3, b >> 3))
            luma = (r * 299 + g * 587 + b * 114) // 1000
            n += 1
            total += luma
            total_sq += luma * luma
    mean = total / n
    return len(colours), max(0.0, total_sq / n - mean * mean) ** 0.5


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--keep-frames", default=None,
                    help="keep the sampled PPMs in this directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    frame_dir = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_drive_")
    os.makedirs(frame_dir, exist_ok=True)
    env = dict(os.environ,
               MDKR_AUDIO="0",            # belt-and-braces; --headless is the guarantee
               MDKR_SIMULATION_CADENCE="enhanced",
               MDKR_SYNTH_FIELDS="1",
               MDKR_AUTOPILOT="1",        # closed-loop: DKR's own AI drives (see docstring)
               MDKR_TRACE="1",
               MDKR_DUMP_FROM=str(RACE_START),
               MDKR_DUMP_EVERY=str(SAMPLE_EVERY))
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--dump-frames", frame_dir, "--rom", args.rom]
    if args.verbose:
        print("$ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr

    failures: list[str] = []
    if proc.returncode != 0:
        failures.append(f"exit code {proc.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"{marker} in output: {line.strip()}")
    sanitizer_match = SANITIZER_RE.search(out)
    if sanitizer_match is not None:
        line = next(
            (line for line in out.splitlines() if SANITIZER_RE.search(line)),
            sanitizer_match.group(0),
        )
        failures.append(f"sanitizer diagnostic in output: {line.strip()}")

    rows = []
    for line in out.splitlines():
        m = PACE_RE.search(line)
        if m:
            rows.append((int(m.group(1)), parse_float(m.group(2)), parse_float(m.group(3)),
                         parse_float(m.group(4)), int(m.group(5)), int(m.group(6)),
                         int(m.group(7))))
    if not rows:
        failures.append("no [PACE] racer rows — the run never reached the race")
        print_result(failures)
        return 1 if failures else 0

    drive = [r for r in rows if r[0] >= RACE_START]
    if len(drive) < 1000:
        failures.append(f"only {len(drive)} in-race frames traced (want >= 1000)")

    # 1. position sanity
    bad_finite = [r for r in drive if not (is_finite(r[1]) and is_finite(r[2]) and is_finite(r[3]))]
    if bad_finite:
        r = bad_finite[0]
        failures.append(f"non-finite position at frame {r[0]}: ({r[1]}, {r[2]}, {r[3]}) "
                        f"({len(bad_finite)} frames affected)")
    y_bad = [r for r in drive if is_finite(r[2]) and not (Y_MIN <= r[2] <= Y_MAX)]
    if y_bad:
        r = min(y_bad, key=lambda r: r[2]) if y_bad[0][2] < Y_MIN else max(y_bad, key=lambda r: r[2])
        failures.append(f"y out of the track band [{Y_MIN}, {Y_MAX}] at frame {r[0]}: y={r[2]}")

    # Step lengths, then the per-frame CHANGE in step length.  A boost ramps (the
    # measured zip-pad burst gains at most 3.5 units/frame of speed per frame); a
    # teleport is a discontinuity, so its acceleration is as large as the jump.
    steps: list[tuple[int, float]] = []
    for a, b in zip(drive, drive[1:]):
        if b[0] != a[0] + 1:
            continue
        if not all(is_finite(v) for v in (a[1], a[2], a[3], b[1], b[2], b[3])):
            continue
        steps.append((b[0], ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2
                             + (b[3] - a[3]) ** 2) ** 0.5))
    max_step, max_step_frame = max(steps, key=lambda s: s[1], default=(None, 0.0))[::-1]
    max_accel, max_accel_frame = 0.0, None
    for (_fa, da), (fb, db) in zip(steps, steps[1:]):
        if abs(db - da) > max_accel:
            max_accel, max_accel_frame = abs(db - da), fb
    if max_step > MAX_STEP:
        failures.append(f"teleport: {max_step:.1f} world units in one frame at "
                        f"frame {max_step_frame} (limit {MAX_STEP})")
    if max_accel > MAX_ACCEL:
        failures.append(f"teleport: step length changed by {max_accel:.1f} world "
                        f"units between consecutive frames at frame "
                        f"{max_accel_frame} (limit {MAX_ACCEL}) — a boost ramps, "
                        f"a discontinuity does not")

    # 2. forward progress
    final_cp, final_lap = drive[-1][5], drive[-1][6]
    if final_cp < MIN_FINAL_CP:
        failures.append(f"only reached checkpoint {final_cp} (want >= {MIN_FINAL_CP}) — "
                        f"the racer is not driving the track")
    if final_lap < MIN_FINAL_LAP:
        failures.append(f"only reached lap {final_lap} (want >= {MIN_FINAL_LAP})")

    worst_mean, worst_frame = None, None
    for i in range(0, len(drive) - STALL_WINDOW, STALL_WINDOW // 2):
        seg = drive[i:i + STALL_WINDOW]
        steps = []
        for a, b in zip(seg, seg[1:]):
            if b[0] == a[0] + 1 and all(is_finite(v) for v in (a[1], a[2], a[3], b[1], b[2], b[3])):
                steps.append(((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2 + (b[3] - a[3]) ** 2) ** 0.5)
        if not steps:
            continue
        mean = sum(steps) / len(steps)
        if worst_mean is None or mean < worst_mean:
            worst_mean, worst_frame = mean, seg[0][0]
    if worst_mean is not None and worst_mean < STALL_MIN_MEAN_SPEED:
        failures.append(f"stalled: mean speed {worst_mean:.2f} units/frame over "
                        f"{STALL_WINDOW} frames from frame {worst_frame} "
                        f"(limit {STALL_MIN_MEAN_SPEED})")

    # 3. the scene still draws
    dumped = sorted(int(re.search(r"frame_(\d+)\.ppm$", f).group(1))
                    for f in os.listdir(frame_dir) if f.endswith(".ppm"))
    if len(dumped) < 4:
        failures.append(f"only {len(dumped)} frames dumped (want >= 4)")
    for f in dumped:
        path = os.path.join(frame_dir, f"frame_{f:04d}.ppm")
        colours, sigma = scene_metrics(path)
        if args.verbose:
            print(f"  frame {f}: distinctColours={colours} lumaSigma={sigma:.1f}")
        if colours < MIN_DISTINCT_COLOURS or sigma < MIN_LUMA_SIGMA:
            failures.append(f"frame {f} is a flat field (distinctColours={colours} "
                            f"lumaSigma={sigma:.1f}; want >= {MIN_DISTINCT_COLOURS} / "
                            f">= {MIN_LUMA_SIGMA}) — the world stopped drawing")

    if args.verbose or failures:
        print(f"  in-race frames: {len(drive)}  final cp={final_cp} lap={final_lap}")
        print(f"  max single-frame step: {max_step:.1f} at frame {max_step_frame}")
        print(f"  max step-to-step acceleration: {max_accel:.1f} at frame "
              f"{max_accel_frame}")
        # None when the run never produced STALL_WINDOW consecutive in-race
        # frames -- which is itself a failure (reported above), so this must
        # print it rather than raise on the format string.
        print(f"  slowest {STALL_WINDOW}-frame mean speed: "
              + ("n/a (too few in-race frames)" if worst_mean is None
                 else f"{worst_mean:.2f} at frame {worst_frame}"))
        print(f"  frames checked for render liveness: {dumped}")

    if args.keep_frames is None:
        shutil.rmtree(frame_dir, ignore_errors=True)
    return print_result(failures)


def print_result(failures: list[str]) -> int:
    if failures:
        print("check_race_drive: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_race_drive: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
