#!/usr/bin/env python3
"""Prove the [SMOOTH-VERDICT] per-surface-class census is real, not vacuous.

Task 2 of the presentation-safety plan adds a census that translates existing
replay clause outcomes (uv_scroll_hold_*, MDKR_REPLAY_HOLD_*) into one
[SMOOTH-VERDICT] row per graded MdkrSurfaceClass. This gate proves three
things about that translation on one Remastered scripted race:

* it actually fires: at least one row for WATER_WAVE, OBJECT_ROOT and
  WORLD_SCROLL;
* it is not vacuous: every row's blend+snap equals its own total, and
  WATER_WAVE graded at least one draw;
* it pins today's known-incomplete truth: the wave surfaces have no
  presentation owner yet (docs/architecture/presentation-interpolation.md,
  residual obligation 0), so WATER_WAVE must currently be all-snap with
  top_reason=NO_OWNER. ``--expect-water-owned`` flips that assertion once a
  later task gives waves a real owner; it defaults to false so this gate
  fails loudly the day that default goes stale instead of quietly passing.

Route: Jungle Falls (level 29), MDKR_LOAD_TRACK-retargeted so the race loads
without menu navigation. Jungle Falls is the tree's own established
UV-scroll/wave witness (see check_presentation_matrix.py's
``UV_SCROLL_TRACK``): its waterfall sheet is an ordinary BHV_TEXTURE_SCROLL
level-model batch (WORLD_SCROLL) and its ping-pong water surface is the
wave-driven, still-unowned content (WATER_WAVE) in the same view. Level 5
(Ancient Lake, check_track_sweep's first track) has water but no distinct
scroller in view during an early lap and was measured to produce zero
WORLD_SCROLL rows; Jungle Falls produces all three classes reliably.

Smoothing is armed with the public ``MDKR_PRESENT_RATE=60`` /
``MDKR_PRESENT_SMOOTHING=interpolate`` pair (present rate above the tick
rate, motion smoothing on) — the same production arm condition
``present_sched_replay_armed()`` requires. The census counters accumulate
unconditionally; only the stderr rows are gated, and only on
``MDKR_SMOOTH_VERDICT=1`` on top of the usual ``MDKR_PRESENT_SCHED_TRACE=1``
that gates the whole trace dump.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_full_3lap_tt.txt"
TRACK = "29"                # Jungle Falls: waterfall scroll + ping-pong water
# Interpolated-replay OPPORTUNITY count is not perfectly reproducible run to
# run even headless (host scheduling noise reaches present_sched's
# stale-vs-interpolated decision) -- WATER_WAVE's 129+ paired-triangle
# batches confirm every run regardless, but the ordinary waterfall scroller
# needs enough opportunities for at least one to land near it. 12,000 frames
# produced a WORLD_SCROLL row in most trials and none in one; 20,000 produced
# one in every trial measured (n=3) with margin (blend+snap already >= 14).
FRAMES = 20000
FATAL_RE = fatal_re(r"\[FX BUG\]", "Assertion", "Validation Error")

ROW_RE = re.compile(
    r"\[SMOOTH-VERDICT\] class=(\S+) blend=(\d+) snap=(\d+) "
    r"top_reason=(\S+)"
)

REQUIRED_CLASSES = ("WATER_WAVE", "OBJECT_ROOT", "WORLD_SCROLL")


def environment(save_dir: Path) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=TRACK,
        # Smoothing armed the same way a player reaches it: a present policy
        # above the tick rate plus the public Motion smoothing = Interpolated
        # setting. This is not MDKR_SYNTH_FIELDS -- --headless-frames already
        # drives one deterministic host field opportunity per call, so the
        # whole route is exactly reproducible without any synthetic pacer.
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_SMOOTH_VERDICT="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
    )
    return env


def run(binary: Path, rom: Path, work: Path, timeout: int,
        verbose: bool) -> str:
    import subprocess

    save_dir = work / "save"
    save_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=work,
            env=environment(save_dir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"timed out\n{(exc.stdout or '')[-4000:]}"
        ) from exc

    output = proc.stdout or ""
    fatal = FATAL_RE.search(output)
    if proc.returncode != 0 or fatal is not None:
        raise RuntimeError(
            f"exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}\n{output[-5000:]}"
        )
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--expect-water-owned", action="store_true",
        help=(
            "flip the pinned WATER_WAVE/NO_OWNER assertion once wave "
            "geometry has a real presentation owner (Task 7)"
        ),
    )
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    failures: list[str] = []

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_smooth_verdict_") as temp:
            output = run(binary, rom, Path(temp), args.timeout, args.verbose)
    except (OSError, RuntimeError) as exc:
        print(f"check_smooth_verdict: FAIL — {exc}", file=sys.stderr)
        return 1

    rows: dict[str, tuple[int, int, str]] = {}
    for match in ROW_RE.finditer(output):
        cls, blend_s, snap_s, top_reason = match.groups()
        blend, snap = int(blend_s), int(snap_s)
        # Multiple rows for the same class would mean [SMOOTH-VERDICT] fired
        # more than once per process, which the flush contract (once, at
        # present_sched_trace_summary) forbids.
        if cls in rows:
            failures.append(f"class={cls} printed more than one row")
            continue
        rows[cls] = (blend, snap, top_reason)

    if not rows:
        failures.append("no [SMOOTH-VERDICT] rows at all")

    for cls in REQUIRED_CLASSES:
        if cls not in rows:
            failures.append(f"no [SMOOTH-VERDICT] row for class={cls}")
            continue
        blend, snap, top_reason = rows[cls]
        n = blend + snap
        if n <= 0:
            failures.append(f"class={cls}: blend+snap={n} is not positive")
        if cls == "WATER_WAVE":
            if n <= 0:
                failures.append("class=WATER_WAVE: non-vacuity requires n>0")
            if args.expect_water_owned:
                if snap == n and top_reason == "NO_OWNER":
                    failures.append(
                        "class=WATER_WAVE: --expect-water-owned but the "
                        "surface is still all-snap/NO_OWNER — the pin was "
                        "not flipped by the owner that was supposed to add "
                        "one"
                    )
            else:
                if snap != n or top_reason != "NO_OWNER":
                    failures.append(
                        f"class=WATER_WAVE: expected snap=={n} "
                        f"top_reason=NO_OWNER (today's pinned truth — wave "
                        f"geometry has no presentation owner yet), got "
                        f"blend={blend} snap={snap} "
                        f"top_reason={top_reason}. If a later task gave "
                        f"waves a real owner, this gate needs "
                        f"--expect-water-owned flipped as its new default."
                    )

    if failures:
        print("check_smooth_verdict: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"rows: {rows}", file=sys.stderr)
        return 1

    summary = "; ".join(
        f"{cls} blend={rows[cls][0]} snap={rows[cls][1]} "
        f"top_reason={rows[cls][2]}"
        for cls in REQUIRED_CLASSES
    )
    print(f"check_smooth_verdict: PASS — {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
