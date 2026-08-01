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
                                It is published only from a racer whose
                                playerIndex == PLAYER_TWO, so its presence *is*
                                the evidence that a second human racer exists.
  4. BOTH PLAYERS MOVE        — for EACH player: finite positions, y inside the
                                track band, no discontinuous teleport, real
                                checkpoint/lap progress, and no long stall.
  5. THEY ARE DISTINCT RACERS — the two probe streams must stay apart in world
                                space, so "the same racer traced twice" cannot
                                pass.
  6. BOTH VIEWPORTS DRAW      — the top and bottom halves of each sampled frame
                                are scored SEPARATELY, so a live viewport cannot
                                mask a dead one.

  7. THE POST-RACE FLOW WORKS  — the full race finishes, MENU_RESULTS loads,
                                and results returns to MENU_TRACK_SELECT —
                                mirroring the 3P/4P arms in
                                check_race_multiplayer.py, so a 2P-specific
                                post-race regression can no longer hide.

Reference (deterministic, Ancient Lake, car, MDKR_AUTOPILOT=1, 9600 frames;
both backends pass, reference numbers measured identically on GL and WebGPU —
an unset selector follows the build's current native default): level_load at frame 2491, race
clock starts 2662, 4719 racing frames with both players traced, final cp P1=53 /
P2=39 (both lap 2), max single-frame step 23.3 / 44.7, max step-to-step change
1.6 / 4.0, slowest racing 240-frame mean speed 10.70 / 1.82, racer separation
min 122 / median 3366, and per-half scores 949..2989 distinct colours / sigma
22.1..66.1 over the in-race window (frames 2600-7400).  A blank viewport half
scores 59 colours / sigma 5.9 (the
calibrated broken-frame value from check_race_drive.py), an order of magnitude
below the thresholds here.  Motion is judged only while each racer's own race
clock advances; DKR freezes finished racers for the fade/results sequence.

Usage:
    tests/check_race_2p_split.py [--build build] [--rom baserom.us.v80.z64]
                                 [--renderer gl|webgpu]
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

from harness_utils import resolve_binary

SCRIPT = "tests/input_scripts/race_2p_split.txt"
FRAMES = 9600          # level_load ~2491, clock starts 2662; post-race flow needs
                       # the full race plus the results/track-select transition,
                       # mirroring check_race_multiplayer's 3P/4P arms
RACE_START = 2662      # the frame the race clock starts counting
DUMP_FROM = 2600
SAMPLE_EVERY = 300

MENU_RESULTS = 17
MENU_TRACK_SELECT = 15
MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
VISUAL_LAST = 7400     # racing ends ~7462; score viewport liveness while both
                       # racers are still on track, not the post-race fade

# --- thresholds (measured values are in the module docstring; observations
# re-measured 2026-07-31 on the extended 9,600-frame racing window) ---
MIN_BOTH_FRAMES = 3000          # observed 4719 racing frames
# A real zip-pad boost can sustain >40 units/frame.  Teleports are identified by
# their discontinuous shape, matching check_race_drive.py: the healthy 2026-07-31
# 2P route peaks at 44.7 with max step-to-step change 4.0; the historic broken
# ASSET_MISC_8 route jumped 1296.8 in one frame.
MAX_STEP = 150.0
MAX_ACCEL = 40.0
Y_MIN, Y_MAX = -150.0, 450.0    # observed 6.8 .. 299.6
MIN_FINAL_CP = 20               # observed 53 (P1) / 39 (P2), lap 2
MIN_FINAL_LAP = 1
STALL_WINDOW = 240
STALL_MIN_MEAN_SPEED = 1.5      # observed slowest racing mean 9.85
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


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def read_ppm(path: str) -> tuple[int, int, bytes]:
    data = open(path, "rb").read()
    idx, fields = 0, []
    while len(fields) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        j = idx
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[idx:j])
        idx = j
    idx += 1                      # single whitespace byte before the raster
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[idx:idx + w * h * 3]


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


def track(rows, name, failures):
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
    if final_cp < MIN_FINAL_CP:
        failures.append(f"{name}: only reached checkpoint {final_cp} "
                        f"(want >= {MIN_FINAL_CP}) — this player is not driving the track")
    if final_lap < MIN_FINAL_LAP:
        failures.append(f"{name}: only reached lap {final_lap} (want >= {MIN_FINAL_LAP})")

    worst_mean, worst_frame = None, None
    for i in range(0, len(steps) - STALL_WINDOW, STALL_WINDOW // 2):
        seg = steps[i:i + STALL_WINDOW]
        mean = sum(seg) / len(seg)
        if worst_mean is None or mean < worst_mean:
            worst_mean, worst_frame = mean, drive[i]
    if worst_mean is not None and worst_mean < STALL_MIN_MEAN_SPEED:
        failures.append(f"{name}: stalled: mean speed {worst_mean:.2f} units/frame over "
                        f"{STALL_WINDOW} frames from frame {worst_frame} "
                        f"(limit {STALL_MIN_MEAN_SPEED})")

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
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--renderer", default=None, choices=["gl", "webgpu"],
                    help="force a backend (default: the build's native default, GL)")
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
    env = dict(os.environ,
               MDKR_AUDIO="0",          # belt-and-braces; --headless is the guarantee
               MDKR_SIMULATION_CADENCE="enhanced",
               MDKR_SYNTH_FIELDS="1",
               MDKR_TRACE="1",
               MDKR_AUTOPILOT="1",      # drives BOTH human racers with DKR's own AI
               MDKR_UI_OVERLAY_TRACE="1",
               MDKR_SAVE_DIR=os.path.join(frame_dir, "save"),
               MDKR_DUMP_FROM=str(DUMP_FROM),
               MDKR_DUMP_EVERY=str(SAMPLE_EVERY))
    if args.renderer:
        env["MDKR_RENDERER"] = args.renderer
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--window-size", args.window_size,
           "--input-script", SCRIPT, "--dump-frames", frame_dir, "--rom", args.rom]
    if args.verbose:
        print("$ " + " ".join(f"{k}={env[k]}" for k in
                              ("MDKR_AUDIO", "MDKR_TRACE", "MDKR_AUTOPILOT")) +
              (f" MDKR_RENDERER={args.renderer}" if args.renderer else "") +
              " " + " ".join(cmd))
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
        return print_result(failures)

    # The probes keep publishing stale rows after each racer finishes (the
    # freeze is legitimate post-race state), so every motion/separation
    # quantity is computed over the racing window only.
    p1_racing = racing_rows(p1)
    p2_racing = racing_rows(p2)
    both = [f for f in sorted(set(p1_racing) & set(p2_racing))
            if f >= RACE_START]
    if len(both) < MIN_BOTH_FRAMES:
        failures.append(f"only {len(both)} racing frames with BOTH players "
                        f"traced (want >= {MIN_BOTH_FRAMES})")

    # 4. both players move
    s1 = track(p1, "player 1", failures)
    s2 = track(p2, "player 2", failures)

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
    dumped = [f for f in dumped if f <= VISUAL_LAST]
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

    # 7. the 2P post-race flow reaches RESULTS and returns to TRACK SELECT —
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
        if min_sep is not None:
            print(f"  racer separation: min {min_sep:.1f}  median {med_sep:.1f}")
        print(f"  frames checked for per-viewport render liveness: {dumped}")

    if args.keep_frames is None:
        shutil.rmtree(frame_dir, ignore_errors=True)
    return print_result(failures)


def print_result(failures: list[str]) -> int:
    if failures:
        print("check_race_2p_split: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_race_2p_split: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
