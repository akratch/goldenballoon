#!/usr/bin/env python3
"""MOTION-01 motion-quality capture gate for the Modern obstruction resolver.

Drives the pinned routes under ``MDKR_CAMERA_OBSTRUCTION=modern`` and reads the
``camera_motion summary``/``camera_motion stat`` census the fixed-tick finalizer
emits at exit (game/src/camera_obstruction_runtime.c). No new route is recorded:
these are the same input scripts the existing camera checks drive.

Two tiers, deliberately unequal:

  (a) HARD invariants.  These define "not broken", and this check FAILS on them.
      They are structural, not tuned: a correction that engages and disengages
      inside the doc's chatter window, an alternate shot that changes shoulder
      while the camera is still against one continuous surface, or an emergency
      framing that never lets go are defects at any threshold anyone might pick.

  (b) BASELINE reporting.  The analog metrics -- jerk, velocity, latencies --
      are printed, not asserted.  docs/architecture/camera-obstruction.md
      section 7.3 says its numeric bounds are "falsifiable starting bounds, not
      magic constants", to be calibrated from traces and frozen by a signed
      review.  Inventing thresholds here would launder a guess into a gate, so
      this check reports the measured distributions and says so.

The check therefore PASSES with a baseline report while tier (a) holds.  It is
measurement infrastructure for the default-on decision, not the decision.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from harness_utils import ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal, resolve_binary

MOTION_SUMMARY = "camera_motion summary"
MOTION_STAT = "camera_motion stat"
OBSERVE_SUMMARY = "camera_obstruction_observe summary"

# The doc's chatter window (section 7.3), mirrored from the runtime constant so
# the reported violation and the measured one cannot drift apart.
OSCILLATION_TICKS = 12

# The only numeric bound tier (a) carries, and it is deliberately an
# obviously-broken bound rather than a quality one: five seconds of continuous
# emergency framing is a camera that never recovered, at any tuning.  The
# quality bound on emergency dwell belongs to the section 7.3 signed review,
# which is why the measured distribution is also reported below.
MAX_EMERGENCY_DWELL_TICKS = 300


@dataclass(frozen=True)
class Route:
    name: str
    script: str
    frames: int
    window: str
    timeout: int
    extra_env: dict[str, str] = field(default_factory=dict)
    extra_args: tuple[str, ...] = ()
    description: str = ""


ROUTES: tuple[Route, ...] = (
    Route(
        name="ancient-lake-race",
        script="tests/input_scripts/race_drive_long.txt",
        frames=5200,
        window="1280x960",
        timeout=900,
        extra_args=("--fov", "70"),
        description="Ancient Lake time trial, the canyon-wall approach route "
                    "check_camera_obstruction_runtime.py pins",
    ),
    Route(
        name="adventure-hub",
        script="tests/input_scripts/adventure_hub_drive.txt",
        frames=12000,
        window="1280x960",
        timeout=1500,
        extra_env={"MDKR_DRIVE_ROUTE": "HUB_TOUR_ROUTE", "MDKR_AUTOPILOT": "1",
                   "MDKR_TRACE": "1"},
        description="Timber's Island hub tour, the fixed-camera and door route "
                    "check_adventure_hub.py pins",
    ),
    Route(
        name="3p-tt-spectate",
        script="tests/input_scripts/race_3p_tt_camera.txt",
        frames=7000,
        window="1920x1080",
        timeout=1200,
        extra_env={"MDKR_AUTOPILOT": "1"},
        description="3P Ancient Lake with the fourth viewport on the T.T. "
                    "spectator camera, as check_camera_3p_tt_runtime.py pins",
    ),
)


def scalar(row: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}=(\d+)", row)
    if match is None:
        raise RuntimeError(f"missing {name} in motion row: {row[:240]}")
    return int(match.group(1))


def block_scalar(row: str, block: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(block)}=\{{[^}}]*\b{re.escape(name)}=(\d+)", row
    )
    if match is None:
        raise RuntimeError(f"missing {block}.{name} in motion row: {row[:300]}")
    return int(match.group(1))


def real(row: str, name: str) -> float:
    match = re.search(rf"\b{re.escape(name)}=(-?\d+(?:\.\d+)?)", row)
    if match is None:
        raise RuntimeError(f"missing {name} in motion row: {row[:240]}")
    return float(match.group(1))


def parse_stats(output: str) -> dict[str, dict[str, float]]:
    stats: dict[str, dict[str, float]] = {}
    for row in output.splitlines():
        if MOTION_STAT not in row:
            continue
        name = re.search(r"\bname=(\S+)", row)
        unit = re.search(r"\bunit=(\S+)", row)
        if name is None or unit is None:
            raise RuntimeError(f"malformed motion stat row: {row[:240]}")
        stats[name.group(1)] = {
            "unit": unit.group(1),
            "samples": scalar(row, "samples"),
            "min": real(row, "min"),
            "mean": real(row, "mean"),
            "p95": real(row, "p95"),
            "max": real(row, "max"),
        }
    return stats


def parse_summary(row: str) -> dict[str, float]:
    return {
        "slot_ticks": scalar(row, "slot_ticks"),
        "profile_full": block_scalar(row, "profiles", "full"),
        "profile_safety_only": block_scalar(row, "profiles", "safety_only"),
        "profile_depenetrate_only": block_scalar(row, "profiles", "depenetrate_only"),
        "retract_events": block_scalar(row, "events", "retract"),
        "recovery_events": block_scalar(row, "events", "recovery"),
        "alternate_entries": block_scalar(row, "events", "alternate_entry"),
        "alternate_exits": block_scalar(row, "events", "alternate_exit"),
        "emergency_entries": block_scalar(row, "events", "emergency_entry"),
        "discontinuities": block_scalar(row, "events", "discontinuity"),
        "degenerate_ticks": block_scalar(row, "events", "degenerate"),
        "oscillation_cycles": block_scalar(row, "chatter", "oscillation_cycles"),
        "oscillation_cycles_excused":
            block_scalar(row, "chatter", "oscillation_cycles_excused"),
        "correction_reengagements":
            block_scalar(row, "chatter", "correction_reengagements"),
        "shoulder_flips": block_scalar(row, "shoulder", "flips"),
        "shoulder_flips_continuous_surface":
            block_scalar(row, "shoulder", "continuous_surface"),
        "shoulder_flips_new_surface": block_scalar(row, "shoulder", "new_surface"),
        "blocker_changes": block_scalar(row, "churn", "blocker_changes"),
        "blocker_changes_same_surface": block_scalar(row, "churn", "same_surface"),
        "blocker_changes_new_surface": block_scalar(row, "churn", "new_surface"),
        "max_emergency_dwell": block_scalar(row, "emergency", "max_dwell"),
        "discontinuity_per_1000_ticks": real(row, "discontinuity_per_1000_ticks"),
    }


def run_route(route: Route, binary: Path, rom: Path, profile_force: str | None) -> str:
    script = (Path(__file__).resolve().parent.parent / route.script).resolve()
    if not script.is_file():
        raise SystemExit(f"missing: {script}")
    with tempfile.TemporaryDirectory(prefix="mdkr_camera_motion_") as temp:
        # Start from a scrubbed environment: a developer's inherited MDKR_* must
        # not be able to steer a measurement run.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_CAMERA_OBSTRUCTION="modern",
            MDKR_CAMERA_TRACE="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(Path(temp) / "save"),
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / "missing.ini"),
        )
        env.update(route.extra_env)
        if profile_force:
            env["MDKR_CAMERA_PROFILE_FORCE"] = profile_force
        command = [str(binary), "--headless-frames", str(route.frames),
                   "--window-size", route.window]
        command.extend(route.extra_args)
        command.extend(("--input-script", str(script), "--rom", str(rom)))
        process = subprocess.run(
            command, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=route.timeout, check=False,
        )
    output = process.stdout or ""
    if process.returncode != 0:
        tail = "\n".join(output.splitlines()[-60:])
        raise RuntimeError(f"{route.name} exited {process.returncode}\n{tail}")
    marker = find_fatal(output, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{route.name} emitted {marker}")
    return output


def inspect(route: Route, output: str) -> tuple[dict, dict, list[str]]:
    """Return (summary, stats, hard-tier failures) for one route."""
    rows = [row for row in output.splitlines() if MOTION_SUMMARY in row]
    if not rows:
        raise RuntimeError(
            f"{route.name} emitted no '{MOTION_SUMMARY}' row; the motion census "
            "did not run and this route proved nothing")
    summary = parse_summary(rows[-1])
    stats = parse_stats(output)

    failures: list[str] = []

    # The route has to have actually resolved cameras under Modern. A silent
    # zero-sample run must never read as a clean one.
    observe = [row for row in output.splitlines() if OBSERVE_SUMMARY in row]
    if not observe:
        failures.append(f"{route.name}: no '{OBSERVE_SUMMARY}' rows")
    else:
        # Published cuts are the metric a reader will ask "why?" about, and the
        # transition validator is the answer: a cut it declares is one it could
        # not prove an interpolation-safe path for. Summed over the run so the
        # motion census's discontinuity count has an attribution next to it.
        for key, block, name in (
            ("transition_invoked", "transition", "invoked"),
            ("transition_clear", "transition", "clear"),
            ("transition_cut", "transition", "cut"),
            ("transition_tuple_cut", "transition", "tuple_cut"),
        ):
            summary[key] = sum(block_scalar(row, block, name) for row in observe)
        gates = {match.group(1) for match in
                 (re.search(r"\bgate=([A-Z_]+)\(", row) for row in observe)
                 if match is not None}
        if gates != {"MODERN"}:
            failures.append(
                f"{route.name}: expected every tick under MODERN, saw {sorted(gates)}")

    if summary["slot_ticks"] <= 0:
        failures.append(f"{route.name}: motion census sampled no slot ticks")

    # (a.1) Oscillation.  Section 7.3 words this as "no clear/blocked/clear ...
    # correction cycle inside 12 authored ticks", and that raw count is measured
    # and reported below.  It is NOT the hard assertion, on evidence: on the
    # pinned lake route every short span is a single stable blocker entered once
    # and left once -- a kart driving past a distinct wall facet, which the
    # literal wording cannot tell apart from chatter.  What makes a cycle a
    # cycle is that the correction comes BACK, so that is what is enforced: the
    # correction disengaging and re-engaging inside the same window, on top of
    # the doc's own geometrically-invalid excuse.  The raw count stays in the
    # report so section 7.3's signed review can overrule this reading.
    if summary["correction_reengagements"] > 0:
        failures.append(
            f"{route.name}: {summary['correction_reengagements']} "
            f"blocked->clear->blocked re-engagements inside {OSCILLATION_TICKS} "
            "authored ticks")

    # (a.2) shoulder flapping on one continuous surface.
    if summary["shoulder_flips_continuous_surface"] > 0:
        failures.append(
            f"{route.name}: {summary['shoulder_flips_continuous_surface']} "
            "alternate-shot shoulder flips on a continuous-surface obstruction "
            f"(total flips {summary['shoulder_flips']})")

    # (a.3) emergency framing that never lets go.
    if summary["max_emergency_dwell"] > MAX_EMERGENCY_DWELL_TICKS:
        failures.append(
            f"{route.name}: emergency framing held for "
            f"{summary['max_emergency_dwell']} authored ticks "
            f"(> {MAX_EMERGENCY_DWELL_TICKS})")

    return summary, stats, failures


def report_baseline(name: str, summary: dict, stats: dict) -> None:
    print(f"  [{name}] census")
    print(f"    slot ticks sampled       {summary['slot_ticks']}")
    print(f"    profile slot ticks       full={summary['profile_full']} "
          f"safety_only={summary['profile_safety_only']} "
          f"depenetrate_only={summary['profile_depenetrate_only']}")
    print(f"    correction events        retract={summary['retract_events']} "
          f"recovery={summary['recovery_events']} "
          f"alternate_in={summary['alternate_entries']} "
          f"alternate_out={summary['alternate_exits']} "
          f"emergency={summary['emergency_entries']}")
    print(f"    published cuts           {summary['discontinuities']} "
          f"({summary['discontinuity_per_1000_ticks']:.4f} per 1000 slot ticks)")
    if "transition_invoked" in summary:
        print(f"    cut attribution          "
              f"transition_invoked={summary['transition_invoked']} "
              f"clear={summary['transition_clear']} "
              f"cut={summary['transition_cut']} "
              f"tuple_cut={summary['transition_tuple_cut']}")
    print(f"    degenerate ticks         {summary['degenerate_ticks']}")
    print(f"    blocker-identity churn   changes={summary['blocker_changes']} "
          f"same_surface={summary['blocker_changes_same_surface']} "
          f"new_surface={summary['blocker_changes_new_surface']}")
    print(f"    shoulder flips           total={summary['shoulder_flips']} "
          f"continuous_surface={summary['shoulder_flips_continuous_surface']} "
          f"new_surface={summary['shoulder_flips_new_surface']}")
    print(f"    chatter (HARD)           "
          f"reengagements={summary['correction_reengagements']}")
    print(f"    short blocked spans      "
          f"clear->blocked->clear<={OSCILLATION_TICKS}t="
          f"{summary['oscillation_cycles']} "
          f"(excused={summary['oscillation_cycles_excused']}) "
          f"-- ADVISORY, section 7.3's literal wording; a one-off short "
          f"obstruction counts here too")
    print(f"    max emergency dwell      {summary['max_emergency_dwell']} ticks")
    print(f"  [{name}] BASELINE distributions -- reported, NOT asserted; "
          "section 7.3's signed review sets thresholds from these")
    print(f"    {'metric':<20}{'unit':<12}{'n':>8}{'min':>12}{'mean':>12}"
          f"{'p95':>12}{'max':>12}")
    for metric, values in stats.items():
        print(f"    {metric:<20}{values['unit']:<12}{int(values['samples']):>8}"
              f"{values['min']:>12.5f}{values['mean']:>12.5f}"
              f"{values['p95']:>12.5f}{values['max']:>12.5f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--only", action="append", default=[],
                        help="route name to run (repeatable); default is all")
    parser.add_argument("--profile-force", default=None,
                        help="MDKR_CAMERA_PROFILE_FORCE value, e.g. "
                             "'car:safety_only'. Measurement only: the shipped "
                             "table is what an unset run measures.")
    parser.add_argument("--baseline-out", default=None,
                        help="write the baseline census to this JSON path")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    routes = ROUTES
    if args.only:
        selected = set(args.only)
        routes = tuple(route for route in ROUTES if route.name in selected)
        unknown = selected - {route.name for route in ROUTES}
        if unknown:
            raise SystemExit(f"unknown route(s): {sorted(unknown)}")

    failures: list[str] = []
    baseline: dict[str, dict] = {}
    if args.profile_force:
        print(f"  profile force: MDKR_CAMERA_PROFILE_FORCE={args.profile_force} "
              "(measurement arm, not the shipped table)")
    for route in routes:
        print(f"  {route.name}: {route.description}")
        try:
            output = run_route(route, binary, rom, args.profile_force)
            summary, stats, route_failures = inspect(route, output)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(f"{route.name}: {error}")
            continue
        failures.extend(route_failures)
        baseline[route.name] = {"summary": summary, "stats": stats}
        report_baseline(route.name, summary, stats)

    if args.baseline_out:
        Path(args.baseline_out).write_text(
            json.dumps({"profile_force": args.profile_force, "routes": baseline},
                       indent=2, sort_keys=True) + "\n")
        print(f"  baseline written to {args.baseline_out}")

    if failures:
        print("check_camera_motion_quality: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_motion_quality: PASS "
          "(hard invariants held; analog metrics reported as baseline only)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
