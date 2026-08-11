#!/usr/bin/env python3
"""Opening the pause overlay during an authored animation must freeze it, not erase it.

What went wrong
---------------
Opening the app's pause overlay while a scripted-camera scene was playing turned
the whole screen a single flat colour, and it never came back -- closing the
overlay left the flat colour for the rest of the session.  Two scenes reached by
ordinary play showed it: the new-game intro animation (levelId 36, cutsceneId
15) went flat cyan, and the title screen's camera fly-around (levelId 23) went
flat royal blue.  The colour is each level's own authored sky/background fill,
which is the only thing still being drawn once everything in front of it has
collapsed.

Two later reports exposed the same pause boundary in paths those original arms
did not enter.  An attract-mode racer divided by the zero update rate and made
its vertical position NaN (#28).  The race-intro camera is a per-frame pulse;
the app overlay arrived at the next input boundary after that pulse had been
cleared, so the paused picture snapped behind the kart and the resumed race ran
while the flyover caught up (#29).

The pause hands the whole game a zero update rate.  The animation-path follower
(`func_8001F460`, game/src/objects.c) divides by that rate twice, so a paused
frame produced inf -- or NaN where the path step was zero as well -- and folded
it straight back into the spline's own interpolation parameter.  From the next
frame on every camera and object matrix the scene built was NaN, every triangle
collapsed, and only the background fill survived.  Because the poison lands in
the animation's persistent state, resuming could not undo it.

What this asserts, per arm
--------------------------
  1. **It stays a picture.**  Every frame presented while the overlay is open
     must carry roughly as many distinct colours as the frame at the moment the
     overlay opened -- a flat screen collapses this to 1.
  2. **It is genuinely frozen.**  Each of those frames must also stay within a
     hair of the frame the pause started on.  A scene that keeps animating
     behind the overlay would fail this, so the check cannot be satisfied by
     "not blue" alone.
  3. **It comes back.**  After the overlay closes, the picture must move again,
     which is what the pre-fix session could never do.
  4. **The process exits 0** with no crash marker.

Arms
----
  * ``title`` -- the title screen's scripted camera.  Cheap (1100 ticks) and
    reached by simply booting.
  * ``intro`` -- the new-game intro animation the report came from, reached
    through GAME SELECT -> FILE SELECT -> name entry on a fresh save.
  * ``demo`` -- the title's attract race, with no input script.  All eight
    racers must remain finite while the menu-owned scene keeps its camera alive.
  * ``race-intro`` -- the Ancient Lake start-line flyover, before the authored
    camera gives control back to the player or starts the countdown.

Usage:
    tests/check_overlay_pause_cutscene.py [--build build] [--rom baserom.us.v80.z64]
                                          [--arm title|intro|all] [--keep-frames DIR] [-v]

Runs muted and hidden.  Exit 0 = pass; exit 1 = at least one assertion failed.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

INPUT_SCRIPTS = Path(__file__).resolve().parent / "input_scripts"
INTRO_SCRIPT = INPUT_SCRIPTS / "adventure_hub_drive.txt"
RACE_SCRIPT = INPUT_SCRIPTS / "nav_to_time_trial_race.txt"

FATAL_RE = fatal_re("Segmentation fault", "Abort trap")
OPEN_RE = re.compile(r"^\[overlay-test\] opened at frame (\d+)", re.MULTILINE)
CLOSE_RE = re.compile(r"^\[overlay-test\] closed at frame (\d+)", re.MULTILINE)

# Sampling and tolerances.  The pause hold is exact on most frames; the small
# allowance covers the two-task alternation the presenter walks (measured 1.94
# mean absolute level on the title arm) and a fade already in flight when the
# overlay opens (measured 1.31 on the intro arm).
SAMPLE_TARGET = 20000
HOLD_MEAN_ABS_MAX = 4.0
RESUME_MEAN_ABS_MIN = 5.0
COLOUR_FLOOR = 1000
COLOUR_FRACTION_MIN = 0.40

ARMS = {
    # name: (ticks, open_tick, close_tick, dump_from, dump_every, input_script)
    "title": (1100, 700, 1000, 690, 10, INTRO_SCRIPT),
    "intro": (3400, 2900, 3150, 2890, 10, INTRO_SCRIPT),
    "demo": (3000, 2600, 2800, 2590, 10, None),
    "race-intro": (2900, 2670, 2770, 2660, 10, RACE_SCRIPT),
}


def read_ppm(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    index = 0
    fields: list[bytes] = []
    while len(fields) < 4:
        while data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while data[index:index + 1] not in (b"\n", b""):
                index += 1
            continue
        start = index
        while not data[index:index + 1].isspace():
            index += 1
        fields.append(data[start:index])
    index += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[index:index + width * height * 3]


def sample(path: str) -> list[bytes]:
    width, height, pixels = read_ppm(path)
    count = width * height
    step = max(1, count // SAMPLE_TARGET)
    return [pixels[i * 3:i * 3 + 3] for i in range(0, count, step)]


def mean_abs(a: list[bytes], b: list[bytes]) -> float:
    if not a or len(a) != len(b):
        return float("inf")
    total = sum(
        abs(x[0] - y[0]) + abs(x[1] - y[1]) + abs(x[2] - y[2])
        for x, y in zip(a, b)
    )
    return total / (3.0 * len(a))


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def frames_in(directory: str) -> list[tuple[int, str]]:
    found = []
    for path in glob.glob(os.path.join(directory, "frame_*.ppm")):
        match = re.search(r"frame_(\d+)\.ppm$", path)
        if match:
            found.append((int(match.group(1)), path))
    return sorted(found)


def run_arm(name: str, binary: str, rom: str, keep: str | None,
            timeout: int, verbose: bool) -> list[str]:
    ticks, open_tick, close_tick, dump_from, dump_every, input_script = ARMS[name]
    failures: list[str] = []

    holder = keep or tempfile.mkdtemp(prefix=f"mdkr_pause_cutscene_{name}_")
    root = Path(holder)
    frames_dir = root / "frames"
    for sub in ("prefs", "save", "frames"):
        (root / sub).mkdir(parents=True, exist_ok=True)

    environment = dict(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS=str(ticks),
        MDKR_APP_AUTOPLAY_DUMP_FRAMES=str(frames_dir),
        MDKR_APP_PREFS_DIR=str(root / "prefs"),
        MDKR_APP_SMOKE_WINDOW_SIZE="640x480",
        MDKR_AUDIO="0",
        MDKR_DUMP_FROM=str(dump_from),
        MDKR_DUMP_EVERY=str(dump_every),
        MDKR_NO_CRASH_HANDLER="1",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(root / "save"),
        MDKR_TEST_OVERLAY_OPEN_FRAME=str(open_tick),
        MDKR_TEST_OVERLAY_CLOSE_FRAME=str(close_tick),
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    if input_script is not None:
        environment["MDKR_APP_AUTOPLAY_INPUT_SCRIPT"] = str(input_script)
    env = clean_environment(**environment)
    if verbose:
        print(f"$ [{name}] {binary}  ticks={ticks} open={open_tick} close={close_tick}",
              flush=True)
    process = subprocess.run(
        [binary], cwd=str(root), env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""

    if process.returncode != 0:
        failures.append(f"{name}: process exited {process.returncode}")
    fatal = FATAL_RE.search(output)
    if fatal:
        failures.append(f"{name}: emitted {fatal.group(0)!r}")
    if not OPEN_RE.search(output):
        failures.append(f"{name}: the overlay never reported opening")
    if not CLOSE_RE.search(output):
        failures.append(f"{name}: the overlay never reported closing")
    if failures:
        print(output[-4000:], file=sys.stderr)
        return failures

    captured = frames_in(str(frames_dir))
    held = [(index, path) for index, path in captured
            if open_tick <= index <= close_tick]
    resumed = [(index, path) for index, path in captured if index > close_tick]
    if len(held) < 4:
        failures.append(
            f"{name}: only {len(held)} frames captured across the pause "
            f"({open_tick}..{close_tick}); nothing to judge"
        )
        return failures
    if not resumed:
        failures.append(f"{name}: no frames captured after the overlay closed")
        return failures

    reference = sample(held[0][1])
    reference_colours = len(set(reference))
    colour_floor = max(COLOUR_FLOOR, int(reference_colours * COLOUR_FRACTION_MIN))
    if reference_colours < COLOUR_FLOOR:
        failures.append(
            f"{name}: the frame at the pause boundary (frame {held[0][0]}) already "
            f"has only {reference_colours} distinct colours; the route did not "
            f"reach a drawn scene"
        )
        return failures

    worst_hold = 0.0
    for index, path in held[1:]:
        current = sample(path)
        colours = len(set(current))
        if colours < colour_floor:
            failures.append(
                f"{name}: frame {index} presented while paused collapsed to "
                f"{colours} distinct colours (floor {colour_floor}, "
                f"{reference_colours} at the boundary) -- the scene was erased"
            )
            break
        drift = mean_abs(current, reference)
        worst_hold = max(worst_hold, drift)
        if drift > HOLD_MEAN_ABS_MAX:
            failures.append(
                f"{name}: frame {index} drifted {drift:.3f} mean levels from the "
                f"paused frame (limit {HOLD_MEAN_ABS_MAX}); the pause is not a freeze"
            )
            break

    motion = max(mean_abs(sample(path), reference) for _, path in resumed)
    if motion < RESUME_MEAN_ABS_MIN:
        failures.append(
            f"{name}: after the overlay closed the picture moved only "
            f"{motion:.3f} mean levels (need {RESUME_MEAN_ABS_MIN}); it never resumed"
        )

    if not failures:
        print(
            f"pass {name}: {len(held)} paused frames held within "
            f"{worst_hold:.3f} mean levels at >= {colour_floor} colours "
            f"({reference_colours} at the boundary), then moved {motion:.3f} on resume"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--arm", default="all", choices=("all", *ARMS))
    parser.add_argument("--keep-frames", default=None)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = resolve_binary(args.build)
    rom = os.path.abspath(args.rom)
    for path in (binary, rom, str(INTRO_SCRIPT), str(RACE_SCRIPT)):
        if not os.path.exists(path):
            print(f"FAIL overlay pause cutscene: missing {path}", file=sys.stderr)
            return 1
    binary = os.path.abspath(binary)

    names = tuple(ARMS) if args.arm == "all" else (args.arm,)
    failures: list[str] = []
    for name in names:
        keep = None
        if args.keep_frames:
            keep = os.path.join(args.keep_frames, name)
            os.makedirs(keep, exist_ok=True)
        failures.extend(run_arm(name, binary, rom, keep, args.timeout, args.verbose))

    if failures:
        for failure in failures:
            print(f"FAIL overlay pause cutscene: {failure}", file=sys.stderr)
        return 1
    print("overlay pause cutscene: authored animations freeze and resume intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
