#!/usr/bin/env python3
"""Compare a native racer trace with the real US 1.1 ROM running in ares.

The native trace is the end-of-present, all-racer ``[ORACLE]`` probe. The
historical player-one ``[PACE]`` probe carries no velocity, rng, or finish
state, so it can only be reached by an explicit route opt-in and is mutually
exclusive with the gates that read those fields. The ares trace is compact CSV
read directly from the original game's
RDRAM by the local-only, instrumented emulator. Rows are joined by the race's
own lap-time clock rather than host/present frame: menu loads and VI
presentation have different costs, while the in-game clock is the state
transition being compared.

No ROM bytes, emulator state, or captured frames are written to the repository.
The JSON report contains only numeric observations from one deterministic route.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import sys
from collections import Counter
from dataclasses import dataclass, replace
from pathlib import Path


PACE_RE = re.compile(
    r"\[PACE\] frame=(?P<frame>\d+).*racer "
    r"x=(?P<x>\S+) y=(?P<y>\S+) z=(?P<z>\S+) "
    r"clock=(?P<clock>\d+) cp=(?P<checkpoint>-?\d+) lap=(?P<lap>-?\d+)"
)
ORACLE_RE = re.compile(
    r"\[ORACLE\] frame=(?P<frame>\d+) map=-?\d+ slot=\d+ "
    r"x=(?P<x>\S+) y=(?P<y>\S+) z=(?P<z>\S+) "
    r"xv=(?P<x_velocity>\S+) yv=(?P<y_velocity>\S+) "
    r"zv=(?P<z_velocity>\S+) fvel=(?P<forward_velocity>\S+) "
    r"vel=(?P<velocity>\S+) cp=(?P<checkpoint>-?\d+) next=-?\d+ "
    r"lap=(?P<lap>-?\d+) countlap=-?\d+ fin=(?P<race_finished>-?\d+) "
    r"fpos=(?P<finish_position>-?\d+) ridx=(?P<racer_index>\d+) "
    r"pidx=(?P<player_index>-?\d+) vehicle=-?\d+ grounded=-?\d+ "
    r"clock=(?P<clock>\d+) start=(?P<start_timer>-?\d+) delta=\d+ "
    r"rate=(?P<update_rate>-?\d+) rng=(?P<rng_seed>\d+)"
)


@dataclass(frozen=True)
class State:
    frame: int
    clock: int
    x: float
    y: float
    z: float
    checkpoint: int
    lap: int
    x_velocity: float = 0.0
    y_velocity: float = 0.0
    z_velocity: float = 0.0
    forward_velocity: float = 0.0
    velocity: float = 0.0
    race_finished: int = 0
    finish_position: int = 0
    racer_index: int = 0
    player_index: int = 0
    update_rate: int = 0
    rng_seed: int = 0
    start_timer: int = 0
    framebuffer_serial: int = 0


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.inf
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def finite_state(row: State) -> bool:
    return all(math.isfinite(value) for value in (row.x, row.y, row.z))


def load_native(
    path: Path, racer_index: int, allow_legacy_pace: bool
) -> tuple[list[State], str]:
    oracle: list[State] = []
    sampled: list[State] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = ORACLE_RE.search(line)
        if match is not None:
            if int(match["racer_index"]) != racer_index:
                continue
            oracle.append(
                State(
                    frame=int(match["frame"]),
                    clock=int(match["clock"]),
                    x=float(match["x"]),
                    y=float(match["y"]),
                    z=float(match["z"]),
                    checkpoint=int(match["checkpoint"]),
                    lap=int(match["lap"]),
                    x_velocity=float(match["x_velocity"]),
                    y_velocity=float(match["y_velocity"]),
                    z_velocity=float(match["z_velocity"]),
                    forward_velocity=float(match["forward_velocity"]),
                    velocity=float(match["velocity"]),
                    race_finished=int(match["race_finished"]),
                    finish_position=int(match["finish_position"]),
                    racer_index=int(match["racer_index"]),
                    player_index=int(match["player_index"]),
                    update_rate=int(match["update_rate"]),
                    rng_seed=int(match["rng_seed"]),
                    start_timer=int(match["start_timer"]),
                    framebuffer_serial=int(match["frame"]),
                )
            )
            continue
        match = PACE_RE.search(line)
        if match is None:
            continue
        sampled.append(
            State(
                frame=int(match["frame"]),
                clock=int(match["clock"]),
                x=float(match["x"]),
                y=float(match["y"]),
                z=float(match["z"]),
                checkpoint=int(match["checkpoint"]),
                lap=int(match["lap"]),
            )
        )
    if oracle:
        return oracle, "oracle"
    # An absent [ORACLE] probe is a broken capture, not a reason to score a
    # weaker one. Only a route that declares the legacy probe may reach it.
    if not allow_legacy_pace or racer_index != 0 or not sampled:
        return [], "missing"
    # The native probe lives immediately before the vehicle-specific movement
    # dispatch in racer.c. Its clock is incremented for the current update, but
    # its position/progress still describe the state after the previous update.
    # The following presentation therefore contains the completed state for the
    # preceding clock. The ROM trace is sampled after the complete game loop, so
    # normalize that one-update instrumentation phase here.
    rows = [
        State(
            frame=current.frame,
            clock=previous.clock,
            x=current.x,
            y=current.y,
            z=current.z,
            checkpoint=current.checkpoint,
            lap=current.lap,
        )
        for previous, current in zip(sampled, sampled[1:])
    ]
    return rows, "pace"


def load_ares(path: Path, racer_index: int) -> tuple[list[State], int, bool]:
    rows: list[State] = []
    invalid = 0
    # A trace whose rng column was dropped would otherwise be compared against a
    # substituted zero, so carry the column's presence out with the rows.
    rng_column = False
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        rng_column = "rng_seed" in (reader.fieldnames or [])
        for raw in reader:
            if raw.get("valid") != "1":
                invalid += 1
                continue
            if int(raw["racer_index"]) != racer_index:
                continue
            rows.append(
                State(
                    frame=int(raw["frame"]),
                    clock=int(raw["clock"]),
                    x=float(raw["x"]),
                    y=float(raw["y"]),
                    z=float(raw["z"]),
                    checkpoint=int(raw["checkpoint"]),
                    lap=int(raw["lap"]),
                    x_velocity=float(raw["x_velocity"]),
                    y_velocity=float(raw["y_velocity"]),
                    z_velocity=float(raw["z_velocity"]),
                    forward_velocity=float(raw["forward_velocity"]),
                    velocity=float(raw["velocity"]),
                    race_finished=int(raw["race_finished"]),
                    finish_position=int(raw["finish_position"]),
                    racer_index=int(raw["racer_index"]),
                    player_index=int(raw["player_index"]),
                    update_rate=int(raw["logic_update_rate"]),
                    rng_seed=int(raw.get("rng_seed", "0")),
                    start_timer=int(raw["race_start_timer"]),
                    framebuffer_serial=int(raw["framebuffer_serial"]),
                )
            )
    return rows, invalid, rng_column


def normalize_timebase(rows: list[State]) -> tuple[list[State], str]:
    """Return one observation per rendered game update with a usable race clock.

    Normal racers accumulate ``lap_times`` and therefore expose the retail
    clock directly. Boss vehicle wrappers do not. For them, derive the same
    60 Hz tick domain by summing each completed update's logic rate after the
    positive countdown has reached zero.
    """
    if not rows:
        return rows, "missing"

    # The emulator can present the same game framebuffer on multiple VI fields.
    # RDRAM is not frozen while that framebuffer remains on screen: after the
    # first observation, the CPU can begin mutating racer state for the next
    # update without publishing a new framebuffer serial. Retaining the last
    # sample therefore admits an in-flight hybrid (the Ancient Lake witness saw
    # Y move by 0.05 and then settle back at the next serial). Keep the first
    # observation for each serial. by_clock() below still keeps the later serial
    # when the same completed race clock is presented again, which selects the
    # stable post-update state rather than the transient write window.
    collapsed_by_serial: dict[int, State] = {}
    for row in rows:
        if row.framebuffer_serial > 0:
            collapsed_by_serial.setdefault(row.framebuffer_serial, row)
    collapsed = (
        [collapsed_by_serial[key] for key in sorted(collapsed_by_serial)]
        if collapsed_by_serial else rows
    )
    if any(row.clock > 0 for row in collapsed):
        return collapsed, "lap_times"

    saw_countdown = False
    race_started = False
    tick = 0
    normalized: list[State] = []
    for row in collapsed:
        if row.start_timer > 0:
            saw_countdown = True
        elif saw_countdown:
            race_started = True
        if race_started:
            tick += max(1, row.update_rate)
            normalized.append(replace(row, clock=tick))
        else:
            normalized.append(row)
    return normalized, "derived_logic_ticks"


def by_clock(rows: list[State], quantum: int = 1) -> dict[int, State]:
    # A present can repeat an in-game state. Retain the last observation for a
    # clock tick so both runners compare the state after that tick's update.
    return {
        (row.clock + quantum - 1) // quantum: row
        for row in rows
        if row.clock > 0
    }


def first_progress_divergence(
    clocks: list[int], native: dict[int, State], ares: dict[int, State]
) -> dict[str, int] | None:
    for clock in clocks:
        left, right = native[clock], ares[clock]
        if (left.checkpoint, left.lap) != (right.checkpoint, right.lap):
            return {
                "clock": clock,
                "native_frame": left.frame,
                "ares_frame": right.frame,
                "native_checkpoint": left.checkpoint,
                "ares_checkpoint": right.checkpoint,
                "native_lap": left.lap,
                "ares_lap": right.lap,
            }
    return None


def first_position_divergence(
    clocks: list[int],
    native: dict[int, State],
    ares: dict[int, State],
    threshold: float,
) -> dict[str, float | int] | None:
    for clock in clocks:
        left, right = native[clock], ares[clock]
        error = math.sqrt(
            (left.x - right.x) ** 2
            + (left.y - right.y) ** 2
            + (left.z - right.z) ** 2
        )
        if error > threshold:
            return {
                "clock": clock,
                "native_frame": left.frame,
                "ares_frame": right.frame,
                "error": round(error, 6),
            }
    return None


def clock_step_histogram(rows: dict[int, State]) -> dict[str, int]:
    clocks = sorted(rows)
    return {
        str(step): count
        for step, count in sorted(
            Counter(right - left for left, right in zip(clocks, clocks[1:])).items()
        )
    }


def value_histogram(rows: list[State], field: str) -> dict[str, int]:
    return {
        str(value): count
        for value, count in sorted(
            Counter(getattr(row, field) for row in rows).items()
        )
    }


def first_finish_clock(rows: list[State]) -> int | None:
    clocks = [row.clock for row in rows if row.clock > 0 and row.race_finished]
    return min(clocks) if clocks else None


def checkpoint_clocks(rows: dict[int, State]) -> dict[str, int]:
    first: dict[int, int] = {}
    for clock in sorted(rows):
        checkpoint = rows[clock].checkpoint
        if checkpoint >= 0 and checkpoint not in first:
            first[checkpoint] = clock
    return {str(checkpoint): clock for checkpoint, clock in sorted(first.items())}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route", required=True)
    parser.add_argument("--native-log", type=Path, required=True)
    parser.add_argument("--ares-trace", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--racer-index", type=int, default=0)
    parser.add_argument("--require-finish", action="store_true")
    parser.add_argument("--min-common-clocks", type=int, default=1000)
    parser.add_argument("--min-lap", type=int, default=1)
    parser.add_argument("--min-checkpoint", type=int, default=20)
    parser.add_argument("--max-position-p95", type=float, default=5.0)
    parser.add_argument("--max-position-error", type=float, required=True)
    parser.add_argument("--min-progress-agreement", type=float, default=0.98)
    # The three ORACLE-only gates below read fields the legacy probe never
    # records, so they are required exactly when that probe is not in play.
    parser.add_argument("--min-rng-agreement", type=float)
    parser.add_argument("--max-velocity-ratio-deviation", type=float)
    parser.add_argument("--allow-legacy-pace-probe", action="store_true")
    args = parser.parse_args()

    failures: list[str] = []
    if args.min_common_clocks < 1:
        parser.error("--min-common-clocks must be positive")
    if args.racer_index < 0:
        parser.error("--racer-index must be non-negative")
    if args.min_lap < 0 or args.min_checkpoint < 0:
        parser.error("--min-lap and --min-checkpoint must be non-negative")
    for name, value in (
        ("--max-position-p95", args.max_position_p95),
        ("--max-position-error", args.max_position_error),
    ):
        if not math.isfinite(value) or value < 0:
            parser.error(f"{name} must be finite and non-negative")
    if args.max_position_error < args.max_position_p95:
        parser.error("--max-position-error must not be below --max-position-p95")
    if not 0.0 <= args.min_progress_agreement <= 1.0:
        parser.error("--min-progress-agreement must be in 0..1")
    if args.allow_legacy_pace_probe:
        if args.min_rng_agreement is not None:
            parser.error(
                "--min-rng-agreement cannot be measured by the legacy probe"
            )
        if args.max_velocity_ratio_deviation is not None:
            parser.error(
                "--max-velocity-ratio-deviation cannot be measured by the "
                "legacy probe"
            )
    else:
        if args.min_rng_agreement is None:
            parser.error("--min-rng-agreement is required")
        if args.max_velocity_ratio_deviation is None:
            parser.error("--max-velocity-ratio-deviation is required")
        if not 0.0 <= args.min_rng_agreement <= 1.0:
            parser.error("--min-rng-agreement must be in 0..1")
        if (
            not math.isfinite(args.max_velocity_ratio_deviation)
            or args.max_velocity_ratio_deviation < 0
        ):
            parser.error(
                "--max-velocity-ratio-deviation must be finite and non-negative"
            )
    for path in (args.native_log, args.ares_trace):
        if not path.is_file():
            failures.append(f"missing trace: {path}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    native_rows, native_probe = load_native(
        args.native_log, args.racer_index, args.allow_legacy_pace_probe
    )
    ares_rows, ares_invalid, ares_rng_column = load_ares(
        args.ares_trace, args.racer_index
    )
    native_rows, native_clock_basis = normalize_timebase(native_rows)
    ares_rows, ares_clock_basis = normalize_timebase(ares_rows)
    if native_probe == "missing":
        failures.append(
            f"native log has no [ORACLE] racer-index {args.racer_index} probe "
            "(the legacy [PACE] probe is reachable only by route opt-in)"
        )
    elif not native_rows:
        failures.append(
            f"native log contains no racer-index {args.racer_index} state"
        )
    if not ares_rows:
        failures.append(
            f"ares trace contains no valid racer-index {args.racer_index} state"
        )
    if any(not finite_state(row) for row in native_rows):
        failures.append("native trace contains non-finite racer state")
    if any(not finite_state(row) for row in ares_rows):
        failures.append("ares trace contains non-finite racer state")

    comparison_quantum = (
        2
        if native_clock_basis == "derived_logic_ticks"
        and ares_clock_basis == "derived_logic_ticks"
        else 1
    )
    native_clock = by_clock(native_rows, comparison_quantum)
    ares_clock = by_clock(ares_rows, comparison_quantum)
    clocks = sorted(set(native_clock) & set(ares_clock))
    if len(clocks) < args.min_common_clocks:
        failures.append(
            f"only {len(clocks)} common positive race clocks "
            f"(need {args.min_common_clocks})"
        )

    errors: list[float] = []
    exact_progress = 0
    for clock in clocks:
        left, right = native_clock[clock], ares_clock[clock]
        errors.append(
            math.sqrt(
                (left.x - right.x) ** 2
                + (left.y - right.y) ** 2
                + (left.z - right.z) ** 2
            )
        )
        exact_progress += (left.checkpoint, left.lap) == (
            right.checkpoint,
            right.lap,
        )

    progress_agreement = exact_progress / len(clocks) if clocks else 0.0
    position_within = sum(error <= args.max_position_p95 for error in errors)
    position_within_agreement = position_within / len(clocks) if clocks else 0.0
    position_p95 = percentile(errors, 0.95)
    common_motion = [
        (native_clock[clock], ares_clock[clock])
        for clock in clocks
        if not native_clock[clock].race_finished
        and not ares_clock[clock].race_finished
    ]
    native_mean_forward = (
        statistics.fmean(abs(left.forward_velocity) for left, _ in common_motion)
        if common_motion else None
    )
    ares_mean_forward = (
        statistics.fmean(abs(right.forward_velocity) for _, right in common_motion)
        if common_motion else None
    )
    native_mean_object_speed = (
        statistics.fmean(
            math.sqrt(
                left.x_velocity ** 2
                + left.y_velocity ** 2
                + left.z_velocity ** 2
            )
            for left, _ in common_motion
        )
        if common_motion else None
    )
    ares_mean_object_speed = (
        statistics.fmean(
            math.sqrt(
                right.x_velocity ** 2
                + right.y_velocity ** 2
                + right.z_velocity ** 2
            )
            for _, right in common_motion
        )
        if common_motion else None
    )
    rng_exact = sum(
        native_clock[clock].rng_seed == ares_clock[clock].rng_seed
        for clock in clocks
    )
    native_finish_clock = first_finish_clock(native_rows)
    ares_finish_clock = first_finish_clock(ares_rows)
    if args.require_finish and (
        native_finish_clock is None or ares_finish_clock is None
    ):
        failures.append(
            "target racer did not finish in both runners "
            f"(native={native_finish_clock}, ares={ares_finish_clock})"
        )
    if position_p95 > args.max_position_p95:
        failures.append(
            f"position p95 is {position_p95:.3f} world units "
            f"(limit {args.max_position_p95:.3f})"
        )
    if progress_agreement < args.min_progress_agreement:
        failures.append(
            f"checkpoint/lap agreement is {progress_agreement:.3%} "
            f"(need {args.min_progress_agreement:.3%})"
        )
    position_max = max(errors) if errors else math.inf
    if position_max > args.max_position_error:
        failures.append(
            f"worst position error is {position_max:.3f} world units "
            f"(limit {args.max_position_error:.3f})"
        )

    rng_agreement = rng_exact / len(clocks) if clocks else 0.0
    object_speed_ratio = (
        native_mean_object_speed / ares_mean_object_speed
        if native_mean_object_speed is not None
        and ares_mean_object_speed not in (None, 0.0)
        else None
    )
    if not args.allow_legacy_pace_probe:
        # A trace whose seeds are all zero would agree with another all-zero
        # trace perfectly, so the seeds must exist before the ratio means
        # anything.
        if not ares_rng_column:
            failures.append("ares trace has no rng_seed column")
        elif not any(row.rng_seed for row in ares_rows):
            failures.append("ares trace carries no non-zero rng seed")
        if not any(row.rng_seed for row in native_rows):
            failures.append("native trace carries no non-zero rng seed")
        if rng_agreement < args.min_rng_agreement:
            failures.append(
                f"rng agreement is {rng_agreement:.3%} "
                f"(need {args.min_rng_agreement:.3%})"
            )
        if object_speed_ratio is None:
            failures.append(
                "no common unfinished clocks carry comparable object velocity"
            )
        elif abs(object_speed_ratio - 1.0) > args.max_velocity_ratio_deviation:
            failures.append(
                f"object-speed ratio is {object_speed_ratio:.6f} "
                f"(limit 1 +/- {args.max_velocity_ratio_deviation:.6f})"
            )

    max_native_lap = max((row.lap for row in native_rows), default=-1)
    max_ares_lap = max((row.lap for row in ares_rows), default=-1)
    max_native_checkpoint = max(
        (row.checkpoint for row in native_rows), default=-1
    )
    max_ares_checkpoint = max((row.checkpoint for row in ares_rows), default=-1)
    if min(max_native_lap, max_ares_lap) < args.min_lap:
        failures.append(
            f"route did not drive a lap in both runners "
            f"(native={max_native_lap}, ares={max_ares_lap})"
        )
    if min(max_native_checkpoint, max_ares_checkpoint) < args.min_checkpoint:
        failures.append(
            f"route did not cross enough checkpoints in both runners "
            f"(native={max_native_checkpoint}, ares={max_ares_checkpoint})"
        )

    report = {
        "schema": "mdkr64.oracle.state-report.v1",
        "route": args.route,
        "racer_index": args.racer_index,
        "native_probe": native_probe,
        "clock_basis": {
            "native": native_clock_basis,
            "ares": ares_clock_basis,
            "comparison_quantum": comparison_quantum,
        },
        "native_rows": len(native_rows),
        "ares_rows": len(ares_rows),
        "ares_invalid_rows": ares_invalid,
        "common_positive_clocks": len(clocks),
        "clock_range": [clocks[0], clocks[-1]] if clocks else [],
        "native_max_checkpoint": max_native_checkpoint,
        "ares_max_checkpoint": max_ares_checkpoint,
        "native_max_lap": max_native_lap,
        "ares_max_lap": max_ares_lap,
        "progress_exact_rows": exact_progress,
        "progress_agreement": round(progress_agreement, 8),
        "clock_step_histogram": {
            "native": clock_step_histogram(native_clock),
            "ares": clock_step_histogram(ares_clock),
        },
        "logic_update_rate_histogram": {
            "native": value_histogram(native_rows, "update_rate"),
            "ares": value_histogram(ares_rows, "update_rate"),
        },
        "rng_agreement": {
            "exact_rows": rng_exact,
            "agreement": round(rng_agreement, 8),
            "ares_column_present": ares_rng_column,
        },
        "race_outcome": {
            "native_finish_clock": native_finish_clock,
            "ares_finish_clock": ares_finish_clock,
            "native_finish_position": next(
                (row.finish_position for row in native_rows if row.race_finished),
                None,
            ),
            "ares_finish_position": next(
                (row.finish_position for row in ares_rows if row.race_finished),
                None,
            ),
        },
        "checkpoint_first_clock": {
            "native": checkpoint_clocks(native_clock),
            "ares": checkpoint_clocks(ares_clock),
        },
        "forward_velocity": {
            "native_mean_abs": (
                round(native_mean_forward, 6)
                if native_mean_forward is not None else None
            ),
            "ares_mean_abs": (
                round(ares_mean_forward, 6)
                if ares_mean_forward is not None else None
            ),
            "native_over_ares": (
                round(native_mean_forward / ares_mean_forward, 8)
                if native_mean_forward is not None
                and ares_mean_forward not in (None, 0.0) else None
            ),
        },
        "object_velocity": {
            "native_mean_magnitude": (
                round(native_mean_object_speed, 6)
                if native_mean_object_speed is not None else None
            ),
            "ares_mean_magnitude": (
                round(ares_mean_object_speed, 6)
                if ares_mean_object_speed is not None else None
            ),
            "native_over_ares": (
                round(object_speed_ratio, 8)
                if object_speed_ratio is not None else None
            ),
        },
        "first_progress_divergence": first_progress_divergence(
            clocks, native_clock, ares_clock
        ),
        "position_error": {
            "mean": round(statistics.fmean(errors), 6) if errors else None,
            "median": round(statistics.median(errors), 6) if errors else None,
            "p95": round(position_p95, 6) if errors else None,
            "max": round(max(errors), 6) if errors else None,
            "rows_within_limit": position_within,
            "agreement_within_limit": round(position_within_agreement, 8),
            "first_over_limit": first_position_divergence(
                clocks,
                native_clock,
                ares_clock,
                args.max_position_p95,
            ),
        },
        "thresholds": {
            "min_common_clocks": args.min_common_clocks,
            "min_lap": args.min_lap,
            "min_checkpoint": args.min_checkpoint,
            "max_position_p95": args.max_position_p95,
            "max_position_error": args.max_position_error,
            "min_progress_agreement": args.min_progress_agreement,
            "min_rng_agreement": args.min_rng_agreement,
            "max_velocity_ratio_deviation": args.max_velocity_ratio_deviation,
            "racer_index": args.racer_index,
            "require_finish": args.require_finish,
            "allow_legacy_pace_probe": args.allow_legacy_pace_probe,
        },
        "result": "FAIL" if failures else "PASS",
        "failures": failures,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(
        f"state oracle: clocks={len(clocks)} "
        f"progress={progress_agreement:.3%} "
        f"position p95={position_p95:.3f} "
        f"laps native/ares={max_native_lap}/{max_ares_lap} "
        f"cp native/ares={max_native_checkpoint}/{max_ares_checkpoint}"
    )
    if failures:
        print("compare_oracle_state: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        print(f"  report: {args.out}")
        return 1
    print("compare_oracle_state: PASS")
    print(f"  report: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
