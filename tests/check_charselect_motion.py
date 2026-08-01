#!/usr/bin/env python3
"""Prove the character-select dancers are actually dancing -- and stay that way.

Why this exists
----------------
A user once reported the character-select dancers were static. The report was
disproven by ad-hoc analysis with `tools/anim_period.py`: byte-identical
captures across 86 commits, whole-screen motion RMS ~14.1, dominant
autocorrelation period ~19-20 frames (`peak_r` ~0.66) -- clearly moving, and
clearly periodic. But that analysis was run by hand once. There was, and until
this file existed still is, ZERO automated coverage: a real regression in
`obj_loop_char_select()` (`game/src/object_functions.c`) or in the
`music_animation_fraction()` clock it reads (`platform/stubs_dkr.c`) would ship
completely undetected. This productizes that analysis into a permanent gate.

Why whole-screen RMS, not a per-character crop
------------------------------------------------
`check_menu_anim_rate.py` already measures a cropped "two character rows"
region (0.1,0.2,0.95,0.65) for a *different* purpose -- catching the T1 bug
where the dance ran 8x too fast. That crop also catches butterflies drifting
across the screen, which the check's own docstring calls out as a confound for
period detection: on this capture the cropped region's autocorrelation peaks
at lag 9 (r=0.74), not the dancers' true ~19-20 frame cycle, because the
butterflies have their own faster rhythm. Measuring the WHOLE frame instead
reproduces the original ad-hoc numbers almost exactly (14.285 vs 14.120 RMS,
lag 19 vs "period 20", on a capture starting 50 frames later than the original
run) and is what this gate uses for its regression floor.

Whole-screen RMS still has to prove it can catch a dancer-specific freeze, not
just a fully-dead screen (a bug could plausibly freeze only the character
models while the background/butterflies keep moving). Measured directly:
freezing ONLY the check_menu_anim_rate crop (dancers held at their frame-0
pose, everything else including butterflies left alone) drops per-window
(24-frame) whole-screen motion from a real 5.76-6.90 down to 4.24-4.65 -- a
clean, consistent gap below the MIN_WINDOW_MOTION floor chosen here. A full
freeze (below) drops it to exactly 0. So the floor is set with margin against
BOTH failure shapes, and the shipped broken-direction control below exercises
the stronger (full-freeze) one, per the plan's own suggested construction.

The six-window ensemble
------------------------
Reused directly from `check_mip_motion.py`: a single window's motion swings
with capture phase (see that file's WINDOW_FRAMES/WINDOW_COUNT comment), so
one window characterises a frame choice, not the animation. This gate captures
the same CAPTURE_COUNT = WINDOW_FRAMES * WINDOW_COUNT = 144 frames, scores each
24-frame window independently with `tools/anim_period.py`'s own
`autocorrelation()` helper, and asserts the ensemble: mean motion RMS >= floor
AND at least 5/6 windows individually clear a per-window floor. Measured on two
different capture offsets (CAPTURE_FROM 1650 and 1710, i.e. different dance
phase at the first frame), every real window landed in 5.76-6.90 and every
ensemble mean in 6.4-6.7 -- comfortably above both floors with room to spare
before the dancer-freeze ceiling of 4.65.

Periodicity is asserted too, but loosely, per the instruction not to overfit a
flaky measurement: the full 144-frame capture's autocorrelation peak must land
in a generous [14, 26]-frame band with r >= 0.3. Measured real values: lag 19,
r=0.66 (offset 1650) and lag 19, r=0.66 (offset 1710) -- stable across phase,
so the band has real margin without being tuned to the exact ROM value.

Broken-direction control (required)
------------------------------------
No env-gated animation-freeze hook exists anywhere in the game/engine sources
(searched `obj_loop_char_select`, `music_animation_fraction`, and every
FREEZE-named symbol in the tree -- the only FREEZE hits are DPC/RDP bus flags
and an unrelated matrix-registry perf counter). Adding one would be new game
code for a test-only need, which the plan explicitly asks to avoid when an
analyzer-level control will do. It will: the ensemble scorer above is a pure
function of a list of luma grids, so a frozen-frame arm is built by taking the
SAME captured grids and replacing every one of them with a copy of the first
frame -- literally the "capture where every frame is duplicated from frame 0"
construction the plan calls out. That arm is run through the identical scoring
function used for the real capture, and the check's own PASS depends on that
control failing: motion collapses to exactly 0.0 in every window (measured),
which trips both the per-window and ensemble-mean floors and reports "STILL"
for periodicity. If the frozen arm were ever to pass, this script fails loudly
on that alone -- the gate would not be able to tell dancing from static, which
is precisely the coverage hole this file exists to close.

Usage
-----
    MDKR_AUDIO=0 python3 tests/check_charselect_motion.py --build build --rom baserom.us.v80.z64 -v
    tests/check_charselect_motion.py --keep-frames /tmp/charselect   # inspect the real capture

Always muted + headless, per tests/README.md. Exit 0 = pass.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_character_select.txt"

# Character select settles well before this (menu_init: menuId=3 fires ~1394-
# 1600 depending on route bookkeeping -- see tests/README.md's nav_* table and
# check_menu_anim_rate.py). 1650 leaves a comfortable margin past the fade-in.
CAPTURE_FROM = 1650
WINDOW_FRAMES = 24
WINDOW_COUNT = 6
CAPTURE_COUNT = WINDOW_FRAMES * WINDOW_COUNT
FRAMES = CAPTURE_FROM + CAPTURE_COUNT
WINDOW_SIZE = "320x240"  # 640x480 framebuffer, same host convention as check_mip_motion.py
GRID_W, GRID_H = 24, 18  # tools/anim_period.py's default coarse luma grid

MIN_LAG = 3
WINDOW_MAX_LAG = 20  # a 24-frame window cannot see a longer period than this
FULL_MAX_LAG = 40    # generous headroom around the measured ~19-20 frame period

# See "Why whole-screen RMS" above for the measured gap this floor sits in:
# real per-window 5.76-6.90, dancer-only-freeze per-window 4.24-4.65, full
# freeze exactly 0.0.
MIN_WINDOW_MOTION = 5.0
MIN_MEAN_MOTION = 5.5
MIN_POSITIVE_WINDOWS = WINDOW_COUNT - 1  # allow one noisy window, same as check_mip_motion.py

PERIOD_MIN = 14
PERIOD_MAX = 26
PERIOD_MIN_R = 0.3

MENU_RE = re.compile(r"menu_init: menuId=(\d+)")
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion"
)
EXPECTED_MENU = 3  # CHARACTER_SELECT, per tests/README.md


def load_anim_period():
    path = ROOT / "tools" / "anim_period.py"
    spec = importlib.util.spec_from_file_location("anim_period", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def environment(backend: str) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_DUMP_FROM=str(CAPTURE_FROM),
        MDKR_DUMP_EVERY="1",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    return env


def capture(
    binary: Path,
    rom: Path,
    backend: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Path:
    run_dir = work / backend
    dump_dir = run_dir / "frames"
    dump_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames",
        str(FRAMES),
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(dump_dir),
        "--rom",
        str(rom),
        "--window-size",
        WINDOW_SIZE,
    ]
    if verbose:
        print(f"$ ({backend}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{backend}: timed out\n{(exc.stdout or '')[-4000:]}"
        ) from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    menus = [int(m.group(1)) for m in MENU_RE.finditer(output)]
    dumps = sorted(dump_dir.glob("*.ppm"))
    if (
        proc.returncode != 0
        or fatal is not None
        or not menus
        or menus[-1] != EXPECTED_MENU
        or len(dumps) != CAPTURE_COUNT
    ):
        raise RuntimeError(
            f"{backend}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"menus={menus}, dumps={len(dumps)} (want {CAPTURE_COUNT})\n"
            f"{output[-4000:]}"
        )
    return dump_dir


def score(anim_period, grids: list, label: str) -> tuple[list[str], dict]:
    """Score one sequence of coarse luma grids through the same ensemble gate.

    Shared by the real capture (which must pass) and the frozen-frame control
    (which must not) -- see the module docstring's broken-direction section.
    """
    failures: list[str] = []
    window_motions: list[float] = []
    for w in range(WINDOW_COUNT):
        lo, hi = w * WINDOW_FRAMES, (w + 1) * WINDOW_FRAMES
        sub = grids[lo:hi]
        _corr, motion = anim_period.autocorrelation(sub, MIN_LAG, WINDOW_MAX_LAG)
        window_motions.append(motion)

    mean_motion = sum(window_motions) / len(window_motions)
    positive = sum(1 for m in window_motions if m >= MIN_WINDOW_MOTION)

    if mean_motion < MIN_MEAN_MOTION:
        failures.append(
            f"{label}: ensemble mean motion RMS {mean_motion:.3f} < floor "
            f"{MIN_MEAN_MOTION:.2f}"
        )
    if positive < MIN_POSITIVE_WINDOWS:
        failures.append(
            f"{label}: only {positive}/{WINDOW_COUNT} windows at or above the "
            f"per-window motion floor {MIN_WINDOW_MOTION:.2f} (want >= "
            f"{MIN_POSITIVE_WINDOWS}) -- window RMS "
            + " ".join(f"{m:.2f}" for m in window_motions)
        )

    full_corr, full_motion = anim_period.autocorrelation(grids, MIN_LAG, FULL_MAX_LAG)
    pk = anim_period.peaks(full_corr)
    period = pk[0][0] if pk else None
    peak_r = pk[0][1] if pk else 0.0
    # Only meaningful to assert a period once there is real motion to have
    # one; a below-floor arm (e.g. the frozen control) already fails above.
    if full_motion >= MIN_MEAN_MOTION:
        if period is None or not (PERIOD_MIN <= period <= PERIOD_MAX) or peak_r < PERIOD_MIN_R:
            failures.append(
                f"{label}: no plausible dance period in [{PERIOD_MIN},{PERIOD_MAX}] "
                f"frames (got period={period}, peak_r={peak_r:.2f}, want "
                f"r>={PERIOD_MIN_R:.2f})"
            )

    info = dict(
        window_motions=window_motions,
        mean_motion=mean_motion,
        positive=positive,
        full_motion=full_motion,
        period=period,
        peak_r=peak_r,
    )
    return failures, info


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--backend",
        default="gl",
        choices=("gl", "webgpu"),
        help="rendering backend (default: gl; motion detection is renderer-"
        "agnostic, backend parity is check_renderer_backends.py's job)",
    )
    parser.add_argument(
        "--keep-frames",
        help="retain the captured frame sequence under this directory",
    )
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            print(f"FAIL charselect motion: missing {path}", file=sys.stderr)
            return 1

    anim_period = load_anim_period()

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_frames:
        work = Path(args.keep_frames).expanduser().resolve()
        if work.exists():
            print(
                f"FAIL charselect motion: --keep-frames target already exists: {work}",
                file=sys.stderr,
            )
            return 1
        work.mkdir(parents=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr_charselect_motion_")
        work = Path(temporary.name)

    failures: list[str] = []
    try:
        try:
            dump_dir = capture(binary, rom, args.backend, work, args.timeout, args.verbose)
        except (OSError, RuntimeError) as exc:
            print(f"FAIL charselect motion: {exc}", file=sys.stderr)
            return 1

        real_grids, first, last = anim_period.load_capture(
            str(dump_dir), GRID_W, GRID_H, None, None, None
        )
        if len(real_grids) != CAPTURE_COUNT:
            failures.append(
                f"captured {len(real_grids)} frames ({first}..{last}), want "
                f"{CAPTURE_COUNT}"
            )

        real_failures, real_info = score(anim_period, real_grids, "real capture")
        failures.extend(real_failures)
        print(
            f"  real capture: frames {first}..{last}, window RMS "
            + " ".join(f"{m:.2f}" for m in real_info["window_motions"])
            + f" | mean={real_info['mean_motion']:.2f} "
            f"positive={real_info['positive']}/{WINDOW_COUNT}"
        )
        print(
            f"  real capture: whole-run motion RMS={real_info['full_motion']:.3f}, "
            f"period={real_info['period']} frames (peak r={real_info['peak_r']:.2f})"
        )

        # Broken-direction control -- see module docstring. Every frame
        # replaced with a copy of frame 0: an analyzer-level construction,
        # not a new engine hook.
        frozen_grids = [real_grids[0]] * len(real_grids) if real_grids else []
        frozen_failures, frozen_info = score(anim_period, frozen_grids, "frozen control")
        print(
            f"  frozen control: window RMS "
            + " ".join(f"{m:.2f}" for m in frozen_info["window_motions"])
            + f" | mean={frozen_info['mean_motion']:.2f} "
            f"positive={frozen_info['positive']}/{WINDOW_COUNT}"
        )
        if not frozen_failures:
            failures.append(
                "broken-direction control did not fail: a capture where every "
                "frame is duplicated from frame 0 passed the same ensemble gate "
                "the real capture passed -- this check cannot distinguish "
                "dancing from a static screen"
            )
        elif args.verbose:
            print("  frozen control correctly failed:")
            for f in frozen_failures:
                print(f"    - {f}")

        if failures:
            print("FAIL charselect motion:", file=sys.stderr)
            for f in failures:
                print(f"  - {f}", file=sys.stderr)
            if args.keep_frames:
                print(f"  evidence retained in {work}", file=sys.stderr)
            return 1

        print(
            "PASS charselect motion: character select, "
            f"{CAPTURE_COUNT} frames over {WINDOW_COUNT} windows, mean motion "
            f"RMS {real_info['mean_motion']:.2f} (floor {MIN_MEAN_MOTION:.2f}), "
            f"period {real_info['period']} frames; frozen-frame control "
            "correctly rejected"
        )
        if args.keep_frames:
            print(f"  evidence retained in {work}")
        return 0
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
