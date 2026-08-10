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
# The --pan-demote arm needs its own, larger budget than FRAMES: it is not
# enough for a WORLD_SCROLL row to exist (FRAMES above already guarantees
# that with margin) -- it needs at least one CONFIRMED scroller tick to also
# be a tick this route's camera is panning on, and -- per this file's own
# "not perfectly reproducible run to run even headless" note above -- which
# ticks land a confirmed interpolated replay walk at all is itself noisy, so
# that joint event needs real margin. 40,000 frames measured 30-60% failure
# across repeated trials; 100,000 measured WORLD_SCROLL pandemoted > 0 in
# 5/5 trials (1 to 65 demotions each), with OBJECT_ROOT pandemoted == 0 in
# every trial. One of those five also dropped the WATER_WAVE row entirely --
# a pre-existing large-frame-count flake in this same fixture unrelated to
# pan demotion (WATER_WAVE's count does not depend on the demotion clause at
# all) -- so this is the same kind of statistical margin FRAMES/20000 above
# carries for the base arm, not a stronger guarantee.
PAN_DEMOTE_FRAMES = 100000
# Test-only override of the production 22.5 deg/tick threshold (see
# MDKR_TEST_PAN_DEMOTE_YAW_DEG_PER_TICK in gfx_pc_dkr.c): Jungle Falls'
# autopilot route pans hard enough to spike past 22.5 deg/tick only a
# handful of times over an entire 3-lap race (measured max ~150 deg/tick),
# and essentially never exactly on the same tick a confirmed WORLD_SCROLL
# batch also draws. 0.05 deg/tick demotes on ordinary camera-follow motion
# instead of requiring a genuine hard pan, which proves the SAME choke-point
# clause (dkr_replay_resolve_alpha's demotion branch, the class filter, and
# the census wiring) without needing a dedicated hard-pan fixture this tree
# does not have. The production default is untouched by this override --
# it only takes effect once MDKR_SMOOTH_PAN_DEMOTE=1 has already been set.
PAN_DEMOTE_TEST_THRESHOLD_DEG = "0.05"
# The shipped production constant (MDKR_PAN_DEMOTE_YAW_DEG_PER_TICK in
# gfx_pc_dkr.c), duplicated here ONLY as the value this gate expects
# [PAN-DEMOTE]'s report to equal -- the report itself is read from the
# binary's own stderr, not assumed, so a source change to the compiled
# constant that this literal is not also updated to match fails the
# production sub-arm below rather than passing silently.
EXPECTED_PRODUCTION_THRESHOLD_DEG = 22.5
FATAL_RE = fatal_re(r"\[FX BUG\]", "Assertion", "Validation Error")

ROW_RE = re.compile(
    r"\[SMOOTH-VERDICT\] class=(\S+) blend=(\d+) snap=(\d+) "
    r"top_reason=(\S+) pandemoted=(\d+)"
)
PAN_THRESHOLD_RE = re.compile(r"\[PAN-DEMOTE\] armed threshold=([0-9.]+)")
# Task 5: VRR-honest alpha quantization. This fixture drives synthetic
# (non-realtime) pacing via --headless-frames, which
# platform_present_display_quantum_units() declines to quantize onto a grid
# unconditionally (platform_sdl_min.c:4083-4102, the very first branch) --
# long before the new variance check ever runs. So this gate only proves the
# line fires and reports what synthetic pacing has always done: mode=free,
# units=0. It is not a witness for the variance-driven decline itself; that
# needs a real, jittery display (tests/check_pacing_quality.py's realtime
# arm).
ALPHA_QUANTUM_RE = re.compile(
    r"\[ALPHA-QUANTUM\] units=(\d+) variance_ppm=(\d+) mode=(grid|free)"
)

REQUIRED_CLASSES = ("WATER_WAVE", "OBJECT_ROOT", "WORLD_SCROLL")


def environment(save_dir: Path, pan_demote: bool,
                 threshold_override: str | None = None) -> dict[str, str]:
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
    if pan_demote:
        # Task 4's optional pan-rate demotion (default OFF). Jungle Falls is
        # the same fixture check_presentation_matrix.py already established
        # as this tree's UV-scroll/wave witness -- autopilot cornering
        # through it is what drives the camera's per-tick yaw rate.
        env["MDKR_SMOOTH_PAN_DEMOTE"] = "1"
        if threshold_override is not None:
            # The test-only override (see PAN_DEMOTE_TEST_THRESHOLD_DEG
            # above): only set for the exercising sub-arm. The production
            # sub-arm below deliberately leaves this unset so it measures
            # the compiled-in default.
            env["MDKR_TEST_PAN_DEMOTE_YAW_DEG_PER_TICK"] = threshold_override
    return env


def run(binary: Path, rom: Path, work: Path, timeout: int, verbose: bool,
        pan_demote: bool, frames: int,
        threshold_override: str | None = None) -> str:
    import subprocess

    save_dir = work / "save"
    save_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames", str(frames),
        "--input-script", str(SCRIPT),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=work,
            env=environment(save_dir, pan_demote, threshold_override),
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


def parse_rows(output: str,
                failures: list[str]) -> dict[str, tuple[int, int, str, int]]:
    rows: dict[str, tuple[int, int, str, int]] = {}
    for match in ROW_RE.finditer(output):
        cls, blend_s, snap_s, top_reason, pandemoted_s = match.groups()
        blend, snap, pandemoted = int(blend_s), int(snap_s), int(pandemoted_s)
        # Multiple rows for the same class would mean [SMOOTH-VERDICT] fired
        # more than once per process, which the flush contract (once, at
        # present_sched_trace_summary) forbids.
        if cls in rows:
            failures.append(f"class={cls} printed more than one row")
            continue
        rows[cls] = (blend, snap, top_reason, pandemoted)
    return rows


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
    parser.add_argument(
        "--pan-demote", action="store_true",
        help=(
            "Task 4: arm MDKR_SMOOTH_PAN_DEMOTE=1 over the same Jungle Falls "
            "route and prove WORLD_SCROLL sees PAN_RATE_DEMOTED rows while "
            "OBJECT_ROOT still blends (objects are never in the demotable "
            "set). With this flag omitted (the default), the env stays "
            "unset and every observed class must show pandemoted=0 -- the "
            "default-off proof."
        ),
    )
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    failures: list[str] = []

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_smooth_verdict_") as temp:
            frames = PAN_DEMOTE_FRAMES if args.pan_demote else FRAMES
            threshold_override = (
                PAN_DEMOTE_TEST_THRESHOLD_DEG if args.pan_demote else None
            )
            output = run(binary, rom, Path(temp), args.timeout, args.verbose,
                         args.pan_demote, frames, threshold_override)
    except (OSError, RuntimeError) as exc:
        print(f"check_smooth_verdict: FAIL — {exc}", file=sys.stderr)
        return 1

    rows = parse_rows(output, failures)

    if not rows:
        failures.append("no [SMOOTH-VERDICT] rows at all")

    # Task 5: the [ALPHA-QUANTUM] line must fire at the same flush window as
    # [SMOOTH-VERDICT], and this fixture's synthetic (--headless-frames)
    # pacing must report the harness's known-today value: mode=free, units=0
    # (see ALPHA_QUANTUM_RE's comment above for why this is not yet a witness
    # for the variance threshold itself).
    alpha_quantum_match = ALPHA_QUANTUM_RE.search(output)
    if alpha_quantum_match is None:
        failures.append("no [ALPHA-QUANTUM] line at all")
    else:
        aq_units, aq_variance_ppm, aq_mode = alpha_quantum_match.groups()
        if aq_units != "0" or aq_mode != "free":
            failures.append(
                f"[ALPHA-QUANTUM]: expected units=0 mode=free under "
                f"synthetic pacing (declined long before the variance "
                f"check runs), got units={aq_units} "
                f"variance_ppm={aq_variance_ppm} mode={aq_mode}"
            )

    for cls in REQUIRED_CLASSES:
        if cls not in rows:
            failures.append(f"no [SMOOTH-VERDICT] row for class={cls}")
            continue
        blend, snap, top_reason, pandemoted = rows[cls]
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

    if args.pan_demote:
        # Positive proof: with the env armed, the demotable WORLD_SCROLL
        # class actually sees PAN_RATE_DEMOTED rows on this hard-cornering
        # route, while OBJECT_ROOT -- never in
        # dkr_replay_surface_class_pan_demotable's set -- must NOT be
        # demoted at all. Object pairing (ownership) is proven by the
        # existing REQUIRED_CLASSES/n>0 checks above; this only adds the
        # zero-demotion assertion on top of that already-proven pairing.
        if "WORLD_SCROLL" in rows and rows["WORLD_SCROLL"][3] <= 0:
            failures.append(
                "class=WORLD_SCROLL: MDKR_SMOOTH_PAN_DEMOTE=1 but "
                "pandemoted=0 -- the armed route never crossed "
                "MDKR_PAN_DEMOTE_YAW_DEG_PER_TICK"
            )
        if "OBJECT_ROOT" in rows and rows["OBJECT_ROOT"][3] != 0:
            failures.append(
                f"class=OBJECT_ROOT: pandemoted="
                f"{rows['OBJECT_ROOT'][3]} but OBJECT_ROOT is not in the "
                "demotable surface-class set and must never be demoted"
            )

        # Production-threshold sub-arm: everything above used
        # PAN_DEMOTE_TEST_THRESHOLD_DEG, so nothing yet has ever exercised
        # the SHIPPED 22.5 deg/tick default -- a change that broke the
        # compiled default while leaving the override mechanism intact would
        # pass every assertion above undetected. This sub-arm arms
        # MDKR_SMOOTH_PAN_DEMOTE=1 with the override left UNSET, at the
        # cheap base FRAMES budget (no need for PAN_DEMOTE_FRAMES' margin --
        # this sub-arm does not need a demotion to actually fire), and pins
        # two things read from the binary's own output, not assumed: (a) the
        # compiled-in threshold the choke point is actually comparing
        # against, via the one-shot [PAN-DEMOTE] trace
        # dkr_replay_pan_demote_enabled() emits the first time it finds
        # itself armed; (b) that this ordinary route's camera genuinely
        # cannot cross that real threshold, so pandemoted stays 0 for every
        # class -- the fixture's own physical limit, asserted rather than
        # silently relied upon.
        try:
            with tempfile.TemporaryDirectory(
                    prefix="mdkr_smooth_verdict_prodthresh_") as prod_temp:
                prod_output = run(binary, rom, Path(prod_temp), args.timeout,
                                  args.verbose, True, FRAMES, None)
        except (OSError, RuntimeError) as exc:
            failures.append(f"production-threshold sub-arm: {exc}")
            prod_output = ""

        if prod_output:
            threshold_match = PAN_THRESHOLD_RE.search(prod_output)
            if threshold_match is None:
                failures.append(
                    "production-threshold sub-arm: no [PAN-DEMOTE] armed "
                    "threshold row was emitted -- dkr_replay_pan_demote_"
                    "enabled() did not report the compiled threshold"
                )
            else:
                reported = float(threshold_match.group(1))
                if abs(reported - EXPECTED_PRODUCTION_THRESHOLD_DEG) > 1e-6:
                    failures.append(
                        f"production-threshold sub-arm: [PAN-DEMOTE] "
                        f"reported threshold={reported}, expected "
                        f"{EXPECTED_PRODUCTION_THRESHOLD_DEG} -- "
                        "MDKR_PAN_DEMOTE_YAW_DEG_PER_TICK changed in "
                        "gfx_pc_dkr.c without updating "
                        "EXPECTED_PRODUCTION_THRESHOLD_DEG here (or the "
                        "compiled default silently regressed)"
                    )
            prod_failures: list[str] = []
            prod_rows = parse_rows(prod_output, prod_failures)
            failures.extend(f"production-threshold sub-arm: {f}"
                            for f in prod_failures)
            for cls, (_, _, _, pandemoted) in prod_rows.items():
                if pandemoted != 0:
                    failures.append(
                        f"production-threshold sub-arm: class={cls} "
                        f"pandemoted={pandemoted} at the real 22.5 "
                        "deg/tick threshold -- this fixture's route was "
                        "assumed unable to cross it; either the route "
                        "changed or the real threshold no longer holds"
                    )
    else:
        # Default-off proof (Step 3): with the env unset, the choke point's
        # demotion clause can never fire, for ANY class.
        for cls, (blend, snap, top_reason, pandemoted) in rows.items():
            if pandemoted != 0:
                failures.append(
                    f"class={cls}: MDKR_SMOOTH_PAN_DEMOTE is unset but "
                    f"pandemoted={pandemoted} -- default-off is violated"
                )

    if failures:
        print("check_smooth_verdict: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"rows: {rows}", file=sys.stderr)
        return 1

    summary = "; ".join(
        f"{cls} blend={rows[cls][0]} snap={rows[cls][1]} "
        f"top_reason={rows[cls][2]} pandemoted={rows[cls][3]}"
        for cls in REQUIRED_CLASSES
    )
    print(f"check_smooth_verdict: PASS — {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
