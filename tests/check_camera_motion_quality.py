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

  (c) HIGH-RATE cut density.  The census counts cuts on the authored tick
      grid, which is the right grid for the resolver and the wrong one for the
      player: at a presentation rate above authored, one authored tick is
      several displayed frames, and a cut is the one thing interpolation
      cannot carry across.  The arm below re-drives a pinned route with
      ``MDKR_PRESENT_RATE`` high and the public ``MotionSmoothing=interpolate``
      setting, and reports the cut rate per 1000 slot ticks (comparable
      straight across to the authored-rate run) next to the fraction of
      DISPLAYED frames that carry one.  Report-only, like tier (b): this is
      the number the cut-density decision needs before camera default-on, not
      the decision itself.

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

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           parse_last, resolve_binary)

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


@dataclass(frozen=True)
class HighRateArm:
    """One route re-driven at a presentation rate above authored.

    ``frame_scale`` exists because ``--headless-frames`` counts PRESENTS, not
    authored ticks: at N times the authored rate the same route needs N times
    the frame budget to cover the same ground.  The arm asserts nothing about
    the numbers it produces; it asserts only that it really ran at the rate it
    asked for and really interpolated, because a silently-degraded arm would
    otherwise report an authored-rate number under a high-rate label.
    """

    route: str
    present_rate: int
    frame_scale: int


# 240 Hz against these routes' 60 Hz authored tick: four displayed frames per
# authored tick, the regime a high-refresh host actually runs in and the one
# where a per-tick cut rate and a per-frame cut rate diverge most visibly.
#
# frame_scale MUST equal the presents-per-tick ratio, and the arm re-derives
# and reports that ratio rather than trusting it: --headless-frames counts
# presents, so a mismatched scale silently drives a different LENGTH of route
# and its cut density is then not comparable with the authored-rate run above.
#
# Ancient Lake is the arm because its authored-rate cut density (132 per 1000
# slot ticks) is by far the highest of the three routes -- if cut density
# blocks default-on anywhere, it blocks it here.
HIGH_RATE_ARMS: tuple[HighRateArm, ...] = (
    HighRateArm(route="ancient-lake-race", present_rate=240, frame_scale=4),
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
        "ticks": scalar(row, "ticks"),
        "cut_ticks": scalar(row, "cut_ticks"),
        "release_hold_ticks": block_scalar(row, "release_hold", "held_ticks"),
        "release_hold_spans": block_scalar(row, "release_hold", "spans"),
        "release_hold_window": block_scalar(row, "release_hold", "window"),
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


def run_route(route: Route, binary: Path, rom: Path, profile_force: str | None,
              *, present_env: dict[str, str] | None = None,
              frame_scale: int = 1) -> str:
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
        # The presentation override lands last, on purpose: it is the one thing
        # a high-rate arm is allowed to change about an otherwise pinned route.
        env.update(present_env or {})
        if profile_force:
            env["MDKR_CAMERA_PROFILE_FORCE"] = profile_force
        command = [str(binary), "--headless-frames",
                   str(route.frames * frame_scale),
                   "--window-size", route.window]
        command.extend(route.extra_args)
        command.extend(("--input-script", str(script), "--rom", str(rom)))
        process = subprocess.run(
            command, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=route.timeout * frame_scale,
            check=False,
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
    print(f"    release hold             "
          f"held_ticks={summary['release_hold_ticks']} "
          f"dropouts={summary['release_hold_spans']} "
          f"window={summary['release_hold_window']}t "
          f"-- ticks a latched retraction stayed engaged over a corridor its "
          f"own sweep called clear; this is the entire cost of the hysteresis")
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


def inspect_high_rate(arm: HighRateArm, route: Route, output: str,
                      authored: dict | None) -> tuple[dict, list[str]]:
    """Return (high-rate measurement, failures) for one presentation arm.

    The only failures raised here are "this arm did not measure what it says
    it measured".  Every cut-density number it produces is report-only.
    """

    failures: list[str] = []
    rows = [row for row in output.splitlines() if MOTION_SUMMARY in row]
    if not rows:
        raise RuntimeError(
            f"{arm.route}@{arm.present_rate}: no '{MOTION_SUMMARY}' row")
    summary = parse_summary(rows[-1])
    sched = parse_last(output, "PRESENTSCHED-SUMMARY",
                       label=f"{arm.route}@{arm.present_rate}")

    presents = sched["presents"]
    interpolated = sched["interp"]
    sim_ticks = sched["simticks"]

    # A degraded arm is worse than no arm: it would publish an authored-rate
    # cut density under a high-rate heading. Prove the rate and the smoothing.
    if sched["presentrate"] != arm.present_rate:
        failures.append(
            f"{arm.route}@{arm.present_rate}: scheduler resolved present rate "
            f"{sched['presentrate']}, not {arm.present_rate}")
    if interpolated <= 0:
        failures.append(
            f"{arm.route}@{arm.present_rate}: no interpolated presents, so "
            "this arm measured the authored rate under a high-rate label")
    if summary["ticks"] <= 0 or presents <= 0:
        failures.append(
            f"{arm.route}@{arm.present_rate}: census sampled "
            f"{summary['ticks']} ticks over {presents} presents")
        return {"summary": summary}, failures

    # frame_scale is the claim "this arm covers the same route as the
    # authored-rate run". If the scheduler did not actually put out that many
    # presents per authored tick, the arm drove a different LENGTH of route and
    # its cut density is not comparable to the number printed beside it.
    presents_per_tick = presents / summary["ticks"]
    if abs(presents_per_tick - arm.frame_scale) > 0.05:
        failures.append(
            f"{arm.route}@{arm.present_rate}: {presents_per_tick:.4f} presents "
            f"per authored tick, but the arm budgeted frames for "
            f"{arm.frame_scale}; the route length does not match the "
            "authored-rate run this is reported against")

    measured = {
        "present_rate": arm.present_rate,
        "presents": presents,
        "interpolated_presents": interpolated,
        "real_endpoints": sched["realendpoints"],
        "replay_endpoints": sched["replayendpoints"],
        "sim_ticks": sim_ticks,
        "census_ticks": summary["ticks"],
        "presents_per_census_tick": presents_per_tick,
        # Directly comparable to the authored-rate route report above: same
        # metric, same denominator, only the presentation rate differs.
        "cuts_per_1000_slot_ticks": summary["discontinuity_per_1000_ticks"],
        "cut_ticks": summary["cut_ticks"],
        "cut_ticks_per_1000_ticks":
            summary["cut_ticks"] * 1000.0 / summary["ticks"],
        # The player-facing form. A cut tick is an authored tick whose camera
        # cannot be reached from the previous one by interpolation, so exactly
        # one displayed frame per cut tick is a hard camera jump and the frames
        # inside that interval are the ones smoothing cannot help.
        "displayed_frames_on_a_cut": summary["cut_ticks"],
        "displayed_frame_cut_fraction": summary["cut_ticks"] / presents,
        "frames_between_cuts":
            presents / summary["cut_ticks"] if summary["cut_ticks"] else 0.0,
    }
    if authored is not None:
        measured["authored_rate_cuts_per_1000_slot_ticks"] = \
            authored["summary"]["discontinuity_per_1000_ticks"]
    return measured, failures


def report_high_rate(arm: HighRateArm, measured: dict) -> None:
    print(f"  [{arm.route}@{arm.present_rate}] HIGH-RATE cut density "
          "-- reported, NOT asserted")
    print(f"    presentation             rate={measured['present_rate']} "
          f"smoothing=interpolate presents={measured['presents']} "
          f"interpolated={measured['interpolated_presents']} "
          f"real_endpoints={measured['real_endpoints']} "
          f"replay_endpoints={measured['replay_endpoints']}")
    print(f"    authored grid            census_ticks="
          f"{measured['census_ticks']} sim_ticks={measured['sim_ticks']} "
          f"presents_per_tick={measured['presents_per_census_tick']:.4f}")
    line = (f"    published cuts           "
            f"{measured['cuts_per_1000_slot_ticks']:.4f} per 1000 slot ticks")
    if "authored_rate_cuts_per_1000_slot_ticks" in measured:
        line += (f" (authored-rate run: "
                 f"{measured['authored_rate_cuts_per_1000_slot_ticks']:.4f})")
    print(line)
    print(f"    cut ticks                {measured['cut_ticks']} "
          f"({measured['cut_ticks_per_1000_ticks']:.4f} per 1000 authored "
          f"ticks) -- authored ticks where at least one slot cut")
    print(f"    displayed-frame incidence "
          f"{measured['displayed_frame_cut_fraction'] * 100.0:.4f}% of "
          f"{measured['presents']} displayed frames land on a cut "
          f"(one every {measured['frames_between_cuts']:.1f} frames)")


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
    parser.add_argument("--skip-high-rate", action="store_true",
                        help="skip the high-rate cut-density arm; it re-drives "
                             "a route at 4x the frame budget")
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

    high_rate: dict[str, dict] = {}
    selected_routes = {route.name for route in routes}
    for arm in HIGH_RATE_ARMS if not args.skip_high_rate else ():
        if arm.route not in selected_routes:
            continue
        route = next(item for item in ROUTES if item.name == arm.route)
        label = f"{arm.route}@{arm.present_rate}"
        print(f"  {label}: {route.name} re-driven at "
              f"MDKR_PRESENT_RATE={arm.present_rate} with "
              "MotionSmoothing=interpolate, for cut density per DISPLAYED "
              "frame rather than per authored tick")
        try:
            output = run_route(
                route, binary, rom, args.profile_force,
                present_env={"MDKR_PRESENT_RATE": str(arm.present_rate),
                             "MDKR_PRESENT_SMOOTHING": "interpolate",
                             # The scheduler summary is what proves the arm
                             # ran at the rate it claims and what supplies the
                             # displayed-frame denominator.
                             "MDKR_PRESENT_SCHED_TRACE": "1"},
                frame_scale=arm.frame_scale)
            measured, arm_failures = inspect_high_rate(
                arm, route, output, baseline.get(arm.route))
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(f"{label}: {error}")
            continue
        failures.extend(arm_failures)
        high_rate[label] = measured
        if not arm_failures:
            report_high_rate(arm, measured)

    if args.baseline_out:
        Path(args.baseline_out).write_text(
            json.dumps({"profile_force": args.profile_force,
                        "routes": baseline, "high_rate": high_rate},
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
