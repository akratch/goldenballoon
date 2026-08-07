#!/usr/bin/env python3
"""Two-player split-screen race check.

Why this exists
---------------
Every other fixture in tests/ is a ONE-player route, and a one-player build
passes all of them.  Two-player split-screen can break in ways none of them can
see:

  * the second player never joins at all (the input layer only ever fed
    controller port 0, so `charselect_new_player()` could never fire) — the run
    then plays a perfectly healthy ONE-player race and exits 0;
  * the viewport layout stays VIEWPORT_LAYOUT_1_PLAYER, so one camera renders
    full-screen and nothing looks obviously wrong in a whole-frame metric;
  * one of the two viewports renders nothing.  A whole-frame colour/sigma score
    still passes, because the OTHER viewport supplies all the variance.  This is
    the exact shape the WebGPU viewport-containment clamp produced in P3.3, and
    split-screen sets two viewports per frame instead of one.

So this asserts, independently:

  1. EXIT CODE 0 and no [CRASH]/[FATAL].  (The Time-Trial ghost stack overflow
     aborted with nothing at all on stderr — content checks alone miss that.)
  2. SPLIT SCREEN IS REAL     — a `hud_init: hudPlayers=1 numViewports=2` line
                                (gHUDNumPlayers is the 0-indexed viewport layout,
                                so 1 == TWO_PLAYERS) and a two-player
                                `level_load: ... numPlayers=1`.
  3. TWO PLAYERS EXIST        — the [PACE2] player-2 probe must appear at all.
                                It proves that a second human-classified racer
                                object exists. Because this long route enables
                                MDKR_AUTOPILOT, it does NOT prove that P2's
                                physical controller input reached that racer.
  4. BOTH PLAYERS MOVE        — for EACH player: finite positions, y inside the
                                track band, no discontinuous teleport, real
                                checkpoint/lap progress, and no long stall.
  5. THEY ARE DISTINCT RACERS — the two probe streams must stay apart in world
                                space, so "the same racer traced twice" cannot
                                pass.
  6. BOTH VIEWPORTS DRAW      — the top and bottom halves of each sampled frame
                                are scored SEPARATELY, so a live viewport cannot
                                mask a dead one.

  7. RESULTS ARE CLASSIFIED    — the end-of-update oracle proves P1 really wins
                                on lap 3 and the production N-1 rule classifies
                                P2 last on lap 2, with distinct positions.

  8. THE POST-RACE FLOW WORKS  — the full race finishes, MENU_RESULTS loads,
                                and results returns to MENU_TRACK_SELECT —
                                mirroring the 3P/4P arms in
                                check_race_multiplayer.py, so a 2P-specific
                                post-race regression can no longer hide.

Reference (deterministic, Ancient Lake, car, MDKR_AUTOPILOT=1, 9600 frames;
both backends pass and an unset renderer follows the build's native default):

* shipping ``original`` cadence: 2180 racing frames with both players traced,
  final cp P1=53 / P2=39 (both lap 2), max single-frame step 30.1 / 48.1,
  slowest racing 240-frame mean speed 25.39 / 14.59;
* fixed-one-field ``enhanced`` cadence: 4852 racing frames with both players
  traced, final pace cp P1=53 / P2=40 (both lap 2), max single-frame step
  14.7 / 25.2, max step-to-step change 5.3 / 5.5, and slowest racing 240-frame
  mean speed 10.78 / 1.52. At the production finish transition P1 crosses in
  position 1; DKR's N-1-finished rule then classifies the still-racing P2 last
  at lap 2 / position 2.

These references include the retail vehicle-audio RNG calls restored by
``c6fbd94`` and proved against the real-ROM trace.  The earlier 4719-row /
1.82-unit Enhanced reference came from the port's missing car-audio RNG calls;
keeping it would preserve a known-inaccurate random stream.  The refreshed
contract gains checkpoint and trace-length coverage while retaining measured
headroom on the chaotic P2 AI speed window; it does not treat that one metric
as evidence of human control response.

The Enhanced P2 path is a chaotic AI/test-hook lane whose authored last-place
classification is useful end-to-end coverage, not human-input evidence. Exact
P1/P2 controller-to-racer dispatch and response are owned by
``check_2p_human_binding.py`` with ``MDKR_AUTOPILOT`` off. The cadence-specific
floors here therefore protect each AI smoke contract without conflating them.
A blank viewport half scores 59 colours / sigma 5.9 (the
calibrated broken-frame value from check_race_drive.py), an order of magnitude
below the thresholds here.  Motion is judged only while each racer's own race
clock advances; DKR freezes finished racers for the fade/results sequence.

Usage:
    tests/check_race_2p_split.py [--build build] [--rom baserom.us.v80.z64]
                                 [--renderer gl|webgpu]
                                 [--cadence original|enhanced]
                                 [--window-size WIDTHxHEIGHT]
                                 [--keep-frames DIR] [-v]
    tests/check_race_2p_split.py --blank-half-control DIR
        Positive control for assertion 6 only: re-score the PPMs in DIR with one
        half overwritten by a flat fill, and require the check to reject them.

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass, replace

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary

SCRIPT = "tests/input_scripts/race_2p_split.txt"
FRAMES = 9600          # level_load ~2491, clock starts 2662; post-race flow needs
                       # the full race plus the results/track-select transition,
                       # mirroring check_race_multiplayer's 3P/4P arms
RACE_START = 2662      # the frame the race clock starts counting
DUMP_FROM = 2600
SAMPLE_EVERY = 300

MENU_RESULTS = 17
MENU_TRACK_SELECT = 15
ANCIENT_LAKE = 5
MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
# --- cadence-specific gameplay contracts ---------------------------------
# The shipping default and the opt-in historical one-field mode are separate
# products: updateRate=2 moves farther per game pass and finishes sooner. Keep
# both baselines explicit instead of weakening one threshold until both pass.
#
# Sweep finding shape-12 (BUG_CLASS_SWEEP_REPORT.md #5/#6/#7): min_final_cp,
# min_mean_speed and min_both_frames were pinned to 1.5-7% of ONE recorded
# trajectory each. cp/frame-count/window-speed are all downstream of the
# AI racing line, which moves under any legitimate physics/RNG/collision
# change (see check_race_drive.py's own docstring: a ParticleBehaviour stride
# fix alone forced MIN_FINAL_CP 20 -> 15, and the RNG-seed/arctan/trig wave
# moved that route's checkpoint from 53 to 14). A margin that size turns a
# healthy build into a false "not driving the track" / "stalled" / "lost
# player" failure. Restored to check_race_drive.py's own calibration norm —
# a floor set well below the measurement, wide enough to separate "drove
# multiple laps" / "not wedged" / "raced for most of the run" from "went
# nowhere", not to reproduce the measurement itself. min_final_lap (both
# players must complete lap 2) is left as the tight, honest, monotone-
# progress gate it already was — no cadence-shift can silently satisfy it.
CADENCE_CONTRACTS = {
    "original": {
        "synth_fields": "2",
        "visual_last": 4700,
        # Measures "both viewports produced racer state for most of the
        # race", not "this race took >= a specific number of frames" — a
        # faster AI line finishes sooner and must not fail this. ~40%
        # headroom below the 2180 reference (was 2100, 3.7% headroom).
        "min_both_frames": 1300,       # observed 2180
        # Separates "drove the track" from "went nowhere"; cp is a pure
        # function of the AI racing line and is not pinned tighter than
        # that. ~45% of the 53/39 reference (was 52/37, ~1 unit margin).
        "min_final_cp": {"player 1": 25, "player 2": 18},  # observed 53 / 39
        "min_final_lap": 2,
        # An extremum (worst 240-frame window) over a ~5000-frame chaotic
        # trajectory; ~50% headroom catches genuine stall/wedge without
        # reacting to a route shift of a few percent (was 7% headroom).
        "min_mean_speed": {"player 1": 12.0, "player 2": 6.0},  # observed 25.39 / 14.59
    },
    "enhanced": {
        "synth_fields": "1",
        "visual_last": 7400,
        "min_both_frames": 2900,       # observed 4852; ~40% headroom (was 1.9%)
        "min_final_cp": {"player 1": 25, "player 2": 18},  # observed 53 / 40
        "min_final_lap": 2,
        # This is an AI/end-to-end smoke lane, not human-input evidence
        # (check_2p_human_binding.py owns that). P2's reference is only
        # 1.52 units/frame at this cadence, so its floor is scaled down
        # with it rather than sharing player 2's original-cadence floor.
        "min_mean_speed": {"player 1": 5.0, "player 2": 0.6},  # observed 10.78 / 1.52
    },
}

# --- shared thresholds (observations are in the module docstring) ----------
# A real zip-pad boost can sustain >40 units/frame.  Teleports are identified by
# their discontinuous shape, matching check_race_drive.py: the healthy 2026-07-31
# 2P route peaks at 44.7 with max step-to-step change 4.0; the historic broken
# ASSET_MISC_8 route jumped 1296.8 in one frame.
MAX_STEP = 150.0
MAX_ACCEL = 40.0
Y_MIN, Y_MAX = -150.0, 450.0    # observed 6.8 .. 299.6
STALL_WINDOW = 240
MIN_SEPARATION = 20.0           # observed min 122.1 (they share a start grid)
MIN_MEDIAN_SEPARATION = 200.0   # observed racing median ~2100
MIN_DISTINCT_COLOURS = 200      # blank half: 59   observed min: 1061
MIN_LUMA_SIGMA = 12.0           # blank half: 5.9  observed min: 21.3

PACE1_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)")
PACE2_RE = re.compile(
    r"\[PACE2\] frame=(\d+) \| racer2 x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)")
HUD_RE = re.compile(r"hud_init: hudPlayers=(\d+) numViewports=(\d+)")
LOAD_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+)")
UI_RE = re.compile(
    r"\[UI-3\] frame=\d+ active=(\d+) draws=(\d+) lateWorld=(\d+) "
    r".*beginFailures=(\d+)"
)
ORACLE_RE = re.compile(
    r"\[ORACLE\] frame=(\d+) map=(\d+) slot=(\d+) .* "
    r"cp=(-?\d+) next=-?\d+ lap=(-?\d+) countlap=-?\d+ "
    r"fin=(-?\d+) fpos=(-?\d+) ridx=(-?\d+) pidx=(-?\d+)"
)


@dataclass(frozen=True)
class TerminalState:
    frame: int
    checkpoint: int
    lap: int
    finished: int
    position: int
    racer: int
    player: int


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def parse_terminal_states(output: str) -> dict[int, TerminalState]:
    """Latest end-of-update Ancient Lake classification for P1 and P2."""
    states: dict[int, TerminalState] = {}
    for line in output.splitlines():
        match = ORACLE_RE.search(line)
        if match is None or int(match.group(2)) != ANCIENT_LAKE:
            continue
        player = int(match.group(9))
        if player not in (0, 1):
            continue
        states[player] = TerminalState(
            frame=int(match.group(1)),
            checkpoint=int(match.group(4)),
            lap=int(match.group(5)),
            finished=int(match.group(6)),
            position=int(match.group(7)),
            racer=int(match.group(8)),
            player=player,
        )
    return states


def terminal_classification_failures(
    states: dict[int, TerminalState],
) -> list[str]:
    """Validate P1's real finish and P2's authored last-place classification."""
    failures: list[str] = []
    requirements = {
        0: (1, 3),  # P1 wins after crossing the third-lap finish.
        1: (2, 2),  # P2 is classified last after reaching at least lap two.
    }
    for player, (position, min_lap) in requirements.items():
        state = states.get(player)
        label = f"P{player + 1}"
        if state is None:
            failures.append(f"{label}: no terminal [ORACLE] classification")
            continue
        if state.player != player or state.racer != player:
            failures.append(
                f"{label}: terminal player/racer identity is "
                f"{state.player}/{state.racer}, expected {player}/{player}"
            )
        if state.finished != 1:
            failures.append(
                f"{label}: terminal raceFinished={state.finished}, expected 1"
            )
        if state.position != position:
            failures.append(
                f"{label}: terminal finishPosition={state.position}, "
                f"expected {position}"
            )
        if state.lap < min_lap:
            failures.append(
                f"{label}: terminal lap={state.lap}, expected >= {min_lap}"
            )
        if state.frame <= RACE_START:
            failures.append(
                f"{label}: terminal classification precedes the race at "
                f"frame {state.frame}"
            )
    positions = [states[player].position for player in (0, 1)
                 if player in states]
    if len(positions) == 2 and sorted(positions) != [1, 2]:
        failures.append(
            f"terminal positions are {sorted(positions)}, expected distinct 1/2"
        )
    return failures


def terminal_control_failures(
    states: dict[int, TerminalState],
) -> list[str]:
    """Prove missing, swapped, and invalid classifications cannot pass."""
    if terminal_classification_failures(states):
        return []
    controls = {
        "missing-P2": {0: states[0]},
        "swapped-positions": {
            0: replace(states[0], position=2),
            1: replace(states[1], position=1),
        },
        "invalid-P2": {
            0: states[0],
            1: replace(states[1], finished=0, position=0, lap=0),
        },
    }
    failures: list[str] = []
    for name, broken in controls.items():
        if not terminal_classification_failures(broken):
            failures.append(
                f"terminal broken-direction control '{name}' was accepted"
            )
    return failures


def band_metrics(w: int, h: int, px: bytes, y0: int, y1: int) -> tuple[int, float]:
    """(distinct 5-bit-quantized colours, luma std-dev) over one horizontal band."""
    x0, x1 = int(w * 0.15), int(w * 0.85)
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


def half_metrics(path: str, px_override: bytes | None = None):
    """Score the top and bottom viewport SEPARATELY.

    In VIEWPORT_LAYOUT_2_PLAYERS the screen splits horizontally: player 1 owns
    [0, h/2), player 2 owns [h/2, h).  Each band is the middle 0.36..0.96 of its
    own viewport, which drops that viewport's HUD row (place / banana count sit
    at the top of each half) while keeping the whole 3D scene.
    """
    w, h, px = read_ppm(path)
    if px_override is not None:
        px = px_override
    top = band_metrics(w, h, px, int(h * 0.18), int(h * 0.48))
    bot = band_metrics(w, h, px, int(h * 0.68), int(h * 0.98))
    return top, bot


def racing_rows(rows):
    """Trim rows after the racer's clock stops advancing.

    DKR freezes a finished racer for the post-race fade/results sequence, so
    motion assertions must end at the finish line: the racer's own race clock
    (PACE group 5) is the authoritative signal, and it stops exactly there.
    """
    frames = sorted(rows)
    last = None
    for prev, cur in zip(frames, frames[1:]):
        if rows[cur][5] > rows[prev][5]:
            last = cur
    if last is None:
        return rows
    return {f: rows[f] for f in frames if f <= last}


def track(rows, name, failures, contract):
    """Assertion 4 for one player.  rows: {frame: (x, y, z, cp, lap, clock)}.

    Only rows up to the racer's finish (clock still advancing) are judged for
    motion; the post-race freeze is legitimate.
    """
    rows = racing_rows(rows)
    frames = sorted(rows)
    drive = [f for f in frames if f >= RACE_START]
    if not drive:
        failures.append(f"{name}: no in-race probe rows at all")
        return None

    bad = [f for f in drive if not all(is_finite(v) for v in rows[f][:3])]
    if bad:
        failures.append(f"{name}: non-finite position at frame {bad[0]}: "
                        f"{rows[bad[0]][:3]} ({len(bad)} frames affected)")
    y_bad = [f for f in drive if is_finite(rows[f][1]) and not (Y_MIN <= rows[f][1] <= Y_MAX)]
    if y_bad:
        worst = min(y_bad, key=lambda f: rows[f][1]) if rows[y_bad[0]][1] < Y_MIN \
            else max(y_bad, key=lambda f: rows[f][1])
        failures.append(f"{name}: y out of the track band [{Y_MIN}, {Y_MAX}] at "
                        f"frame {worst}: y={rows[worst][1]}")

    max_step, max_step_frame, steps = 0.0, None, []
    step_rows: list[tuple[int, float]] = []
    for f in drive:
        if f - 1 not in rows:
            continue
        a, b = rows[f - 1], rows[f]
        if not all(is_finite(v) for v in a[:3] + b[:3]):
            continue
        d = sum((b[i] - a[i]) ** 2 for i in range(3)) ** 0.5
        steps.append(d)
        step_rows.append((f, d))
        if d > max_step:
            max_step, max_step_frame = d, f
    max_accel, max_accel_frame = 0.0, None
    for (previous_frame, previous_step), (frame, step) in zip(step_rows, step_rows[1:]):
        if frame != previous_frame + 1:
            continue
        accel = abs(step - previous_step)
        if accel > max_accel:
            max_accel, max_accel_frame = accel, frame
    if max_step > MAX_STEP:
        failures.append(f"{name}: teleport: {max_step:.1f} world units in one frame at "
                        f"frame {max_step_frame} (limit {MAX_STEP})")
    if max_accel > MAX_ACCEL:
        failures.append(f"{name}: teleport: step length changed by {max_accel:.1f} world "
                        f"units between consecutive frames at frame {max_accel_frame} "
                        f"(limit {MAX_ACCEL}) — a boost ramps, a discontinuity does not")

    final_cp, final_lap = rows[drive[-1]][3], rows[drive[-1]][4]
    min_final_cp = contract["min_final_cp"][name]
    min_final_lap = contract["min_final_lap"]
    if final_cp < min_final_cp:
        failures.append(f"{name}: only reached checkpoint {final_cp} "
                        f"(want >= {min_final_cp}) — this player is not driving the track")
    if final_lap < min_final_lap:
        failures.append(f"{name}: only reached lap {final_lap} (want >= {min_final_lap})")

    worst_mean, worst_frame = None, None
    for i in range(0, len(steps) - STALL_WINDOW, STALL_WINDOW // 2):
        seg = steps[i:i + STALL_WINDOW]
        mean = sum(seg) / len(seg)
        if worst_mean is None or mean < worst_mean:
            worst_mean, worst_frame = mean, drive[i]
    min_mean_speed = contract["min_mean_speed"][name]
    if worst_mean is not None and worst_mean < min_mean_speed:
        failures.append(f"{name}: stalled: mean speed {worst_mean:.2f} units/frame over "
                        f"{STALL_WINDOW} frames from frame {worst_frame} "
                        f"(limit {min_mean_speed})")

    return dict(frames=len(drive), final_cp=final_cp, final_lap=final_lap,
                max_step=max_step, max_step_frame=max_step_frame,
                max_accel=max_accel, max_accel_frame=max_accel_frame,
                worst_mean=worst_mean, worst_frame=worst_frame)


def blank_half_control(frame_dir: str) -> int:
    """Positive control for assertion 6: prove a dead viewport is REJECTED.

    Overwrite one half of each real frame with a flat fill and require the
    per-half metric to fall below the thresholds.  Without this, "both halves
    scored fine" is unfalsifiable.
    """
    ppms = sorted(f for f in os.listdir(frame_dir) if f.endswith(".ppm"))
    if not ppms:
        print("blank-half control: no PPMs in " + frame_dir)
        return 1
    ok = True
    for name in ppms:
        path = os.path.join(frame_dir, name)
        w, h, px = read_ppm(path)
        for which, (y0, y1) in (("top", (0, h // 2)), ("bottom", (h // 2, h))):
            buf = bytearray(px)
            # Flat fog-brown, i.e. the real "world stopped drawing" colour.
            for y in range(y0, y1):
                row = y * w * 3
                buf[row:row + w * 3] = bytes((110, 82, 48)) * w
            top, bot = half_metrics(path, bytes(buf))
            scored = top if which == "top" else bot
            rejected = scored[0] < MIN_DISTINCT_COLOURS or scored[1] < MIN_LUMA_SIGMA
            print(f"  {name} {which} blanked -> colours={scored[0]} sigma={scored[1]:.1f} "
                  f"{'REJECTED (good)' if rejected else 'ACCEPTED (BAD)'}")
            ok = ok and rejected
    print("blank-half control: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--renderer", default=None, choices=["gl", "webgpu"],
                    help="force a backend (default: the build's native default, WebGPU)")
    ap.add_argument("--cadence", default="original", choices=sorted(CADENCE_CONTRACTS),
                    help="gameplay cadence contract to validate (default: original/shipping)")
    ap.add_argument("--window-size", default="640x480",
                    help="initial drawable used for split-screen coverage (default: 640x480)")
    ap.add_argument("--keep-frames", default=None)
    ap.add_argument("--blank-half-control", default=None, metavar="DIR",
                    help="re-score existing PPMs with one half blanked; expects rejection")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.blank_half_control:
        return blank_half_control(args.blank_half_control)
    if not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.window_size):
        print("FAIL: --window-size must be WIDTHxHEIGHT", file=sys.stderr)
        return 1

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    frame_dir = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_2p_")
    os.makedirs(frame_dir, exist_ok=True)
    contract = CADENCE_CONTRACTS[args.cadence]
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",          # belt-and-braces; --headless is the guarantee
        MDKR_SIMULATION_CADENCE=args.cadence,
        MDKR_SYNTH_FIELDS=contract["synth_fields"],
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",      # drives BOTH human racers with DKR's own AI
        MDKR_ORACLE_STATE="1",   # post-update finish/place classification
        MDKR_UI_OVERLAY_TRACE="1",
        MDKR_SAVE_DIR=os.path.join(frame_dir, "save"),
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_DUMP_EVERY=str(SAMPLE_EVERY),
    )
    if args.renderer:
        env["MDKR_RENDERER"] = args.renderer
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--window-size", args.window_size,
           "--input-script", SCRIPT, "--dump-frames", frame_dir, "--rom", args.rom]
    if args.verbose:
        print("$ " + " ".join(f"{k}={env[k]}" for k in
                              ("MDKR_AUDIO", "MDKR_SIMULATION_CADENCE",
                               "MDKR_SYNTH_FIELDS", "MDKR_TRACE", "MDKR_AUTOPILOT",
                               "MDKR_ORACLE_STATE")) +
              (f" MDKR_RENDERER={args.renderer}" if args.renderer else "") +
              " " + " ".join(cmd))
    # This script's input times are present indices under the selected cadence.
    # Never inherit repo-local mdkr64.ini (a developer may have selected
    # 240/uncapped), because that advances far fewer game ticks before each
    # scripted menu edge. A private nonexistent config is the default schema.
    with tempfile.TemporaryDirectory(prefix="mdkr_2p_config_") as config_root:
        env["MDKR_VIDEO_CONFIG_PATH"] = os.path.join(
            config_root, "mdkr64.ini")
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr

    failures: list[str] = []

    # 1. exit code + crash markers
    if proc.returncode != 0:
        failures.append(f"exit code {proc.returncode}")
        if args.verbose:
            print("  process output:")
            for line in out.splitlines()[-120:]:
                print(f"    {line}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"{marker} in output: {line.strip()}")

    # 2. split screen is real
    layouts = [(int(m.group(1)), int(m.group(2))) for m in
               (HUD_RE.search(l) for l in out.splitlines()) if m]
    if (1, 2) not in layouts:
        failures.append("no `hud_init: hudPlayers=1 numViewports=2` line — the race "
                        f"never entered the 2-player viewport layout (saw {sorted(set(layouts))})")
    loads = [(int(m.group(1)), int(m.group(2))) for m in
             (LOAD_RE.search(l) for l in out.splitlines()) if m]
    if not any(n == 1 for _, n in loads):
        failures.append("no two-player `level_load: ... numPlayers=1` — the level was "
                        f"loaded for a different player count (saw {sorted(set(loads))})")
    ui_rows = [
        tuple(int(value) for value in match.groups())
        for line in out.splitlines()
        if (match := UI_RE.search(line))
    ]
    if not any(active and draws for active, draws, _, _ in ui_rows):
        failures.append("output-resolution UI pass was never active in 2-player mode")
    if any(late for _, _, late, _ in ui_rows):
        failures.append("world primitives were emitted after the 2-player UI pass began")
    if ui_rows and ui_rows[-1][3] != 0:
        failures.append(
            f"output-resolution UI begin failures reached {ui_rows[-1][3]}"
        )

    # 3. both players exist
    p1: dict[int, tuple] = {}
    p2: dict[int, tuple] = {}
    for line in out.splitlines():
        m = PACE1_RE.search(line)
        if m:
            p1[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)),
                                   int(m.group(6)), int(m.group(7)), int(m.group(5)))
        m = PACE2_RE.search(line)
        if m:
            p2[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)),
                                   int(m.group(6)), int(m.group(7)), int(m.group(5)))
    if not p1:
        failures.append("no [PACE] rows — the run never reached a race at all")
    if not p2:
        failures.append("no [PACE2] rows — PLAYER_TWO never published a racer probe, "
                        "i.e. there is no second human racer")
    if not p1 or not p2:
        if args.keep_frames is None:
            shutil.rmtree(frame_dir, ignore_errors=True)
        return print_result(failures, args.cadence)

    # The probes keep publishing stale rows after each racer finishes (the
    # freeze is legitimate post-race state), so every motion/separation
    # quantity is computed over the racing window only.
    p1_racing = racing_rows(p1)
    p2_racing = racing_rows(p2)
    both = [f for f in sorted(set(p1_racing) & set(p2_racing))
            if f >= RACE_START]
    if len(both) < contract["min_both_frames"]:
        failures.append(f"only {len(both)} racing frames with BOTH players "
                        f"traced (want >= {contract['min_both_frames']})")

    # 4. both players move
    s1 = track(p1, "player 1", failures, contract)
    s2 = track(p2, "player 2", failures, contract)

    # Production terminal state is sampled after race_check_finish(), unlike
    # [PACE], which lives inside update_player_racer(). P1 crosses the line;
    # DKR then deliberately classifies the sole remaining racer (P2) last once
    # N-1 racers have finished. This is AI/results coverage, not evidence about
    # the physical P2 input path (AUTOPILOT replaced it above).
    terminal = parse_terminal_states(out)
    failures.extend(terminal_classification_failures(terminal))
    failures.extend(terminal_control_failures(terminal))

    # 5. they are distinct racers — judged over the racing window so the
    # post-finish freeze positions cannot dilute (or spuriously fail) the
    # separation statistics
    seps = [sum((p1[f][i] - p2[f][i]) ** 2 for i in range(3)) ** 0.5 for f in both
            if all(is_finite(v) for v in p1[f][:3] + p2[f][:3])]
    min_sep = med_sep = None
    if seps:
        min_sep, med_sep = min(seps), statistics.median(seps)
        if min_sep < MIN_SEPARATION:
            failures.append(f"the two racers coincide (min separation {min_sep:.1f} < "
                            f"{MIN_SEPARATION}) — the probes may be the same racer twice")
        if med_sep < MIN_MEDIAN_SEPARATION:
            failures.append(f"the two racers never diverge (median separation "
                            f"{med_sep:.1f} < {MIN_MEDIAN_SEPARATION})")

    # 6. both viewports draw — scored only inside the in-race window; frames
    # after the race belong to the post-race menu flow, whose layout is not
    # two racer viewports.
    dumped = sorted(int(re.search(r"frame_(\d+)\.ppm$", f).group(1))
                    for f in os.listdir(frame_dir) if f.endswith(".ppm"))
    dumped = [f for f in dumped if f <= contract["visual_last"]]
    if len(dumped) < 6:
        failures.append(f"only {len(dumped)} frames dumped (want >= 6)")
    for f in dumped:
        path = os.path.join(frame_dir, f"frame_{f:04d}.ppm")
        top, bot = half_metrics(path)
        if args.verbose:
            print(f"  frame {f}: TOP colours={top[0]} sigma={top[1]:.1f}   "
                  f"BOTTOM colours={bot[0]} sigma={bot[1]:.1f}")
        for which, (colours, sigma) in (("top (player 1)", top), ("bottom (player 2)", bot)):
            if colours < MIN_DISTINCT_COLOURS or sigma < MIN_LUMA_SIGMA:
                failures.append(f"frame {f} {which} viewport is a flat field "
                                f"(distinctColours={colours} lumaSigma={sigma:.1f}; want "
                                f">= {MIN_DISTINCT_COLOURS} / >= {MIN_LUMA_SIGMA}) — that "
                                f"viewport is not rendering")

    # 8. the 2P post-race flow reaches RESULTS and returns to TRACK SELECT —
    # 3P and 4P are gated in check_race_multiplayer.py; without this, a
    # 2P-specific post-race regression stayed invisible even though it shares
    # most of the production branch.
    menus = [
        (int(match.group(1)), int(match.group(2)))
        for line in out.splitlines()
        if (match := MENU_RE.search(line))
    ]
    result_frames = [
        frame for menu, frame in menus
        if menu == MENU_RESULTS and frame > RACE_START
    ]
    if not result_frames:
        failures.append(
            f"2P race never reached MENU_RESULTS ({MENU_RESULTS}); saw {menus}"
        )
    elif not any(
        menu == MENU_TRACK_SELECT and frame > result_frames[0]
        for menu, frame in menus
    ):
        failures.append(
            f"2P MENU_RESULTS never returned to MENU_TRACK_SELECT "
            f"({MENU_TRACK_SELECT}); saw {menus}"
        )

    if args.verbose or failures:
        print(f"  viewport layouts seen: {sorted(set(layouts))}")
        print(f"  frames with both players traced: {len(both)}")
        for nm, s in (("player 1", s1), ("player 2", s2)):
            if s:
                print(f"  {nm}: final cp={s['final_cp']} lap={s['final_lap']}  "
                      f"max step {s['max_step']:.1f} @{s['max_step_frame']}  "
                      f"max delta-step {s['max_accel']:.1f} @{s['max_accel_frame']}  "
                      f"slowest {STALL_WINDOW}-frame mean {s['worst_mean']:.2f} "
                      f"@{s['worst_frame']}")
        for player in (0, 1):
            state = terminal.get(player)
            if state:
                print(
                    f"  P{player + 1} terminal: frame={state.frame} "
                    f"cp={state.checkpoint} lap={state.lap} "
                    f"finished={state.finished} position={state.position}"
                )
        if not terminal_classification_failures(terminal):
            print("  terminal controls rejected: missing-P2, swapped-positions, "
                  "invalid-P2")
        if min_sep is not None:
            print(f"  racer separation: min {min_sep:.1f}  median {med_sep:.1f}")
        print(f"  frames checked for per-viewport render liveness: {dumped}")

    if args.keep_frames is None:
        shutil.rmtree(frame_dir, ignore_errors=True)
    return print_result(failures, args.cadence)


def print_result(failures: list[str], cadence: str | None = None) -> int:
    label = "check_race_2p_split" + (f"[{cadence}]" if cadence else "")
    if failures:
        print(label + ": FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(label + ": PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
