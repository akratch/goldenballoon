#!/usr/bin/env python3
"""Direct Enhanced-cadence two-player controller/racer binding gate.

The long 2P fixture uses ``MDKR_AUTOPILOT`` to finish three laps.  That is a
valuable end-to-end flow test, but the hook replaces the human's sampled input
with DKR AI pathing before physics.  An AI lane can therefore stall without
saying anything about controller port 2.

This gate keeps autopilot OFF.  It drives a short first-sector window four
ways (neutral, P1, P2, both) and observes the production human-input dispatch
immediately before native test hooks.  Every arm is repeated on GL/WebGPU and
at 60/120 presentation Hz while the Enhanced simulation stays fixed at one
field/tick.  State, consumed input, racer binding, and per-clock trajectories
must be identical across those presentation-only changes.

The neutral arm is the motion detector's positive control.  Two in-memory
controls additionally remove P2 rows and swap ports; the same binding validator
must reject both, so a missing or cross-wired second player cannot pass merely
because two racer objects exist.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
BASE_SCRIPT = ROOT / "tests/input_scripts/race_2p_human_binding.txt"
TICKS = 3700
DRIVE_START = 2600
DRIVE_END = 3600
# The fixed-ticket input queue commits commands authored for script tick N at
# the next simulation input sample.  Keep this boundary explicit so the gate
# checks the production queue contract instead of silently shifting pulses.
TRACE_TICK_OFFSET = 1
LEFT_WINDOWS = ((2660, 2710), (2900, 2950),
                (3140, 3190), (3380, 3430))
ACTIVE_MIN_PATH = 500.0
ACTIVE_MIN_CHECKPOINT = 1
MAX_STEP = 150.0
Y_MIN, Y_MAX = -150.0, 450.0
# The script injects -80; the production joypad accessor removes the ten-unit
# deadzone before exposing the value consumed by racer physics.
CONSUMED_LEFT_STICK = -70

INPUT_RE = re.compile(
    r"\[RACERINPUT\] tick=(\d+) player=(-?\d+) racer=(-?\d+) "
    r"port=(-?\d+) held=([0-9a-fA-F]+) pressed=([0-9a-fA-F]+) "
    r"released=([0-9a-fA-F]+) sx=(-?\d+) sy=(-?\d+) "
    r"delta=(\d+) countdown=(-?\d+)"
)
PACE1_RE = re.compile(
    r"\[PACE\] frame=\d+.*racer x=(\S+) y=(\S+) z=(\S+) "
    r"clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
PACE2_RE = re.compile(
    r"\[PACE2\] frame=\d+ \| racer2 x=(\S+) y=(\S+) z=(\S+) "
    r"clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
SCHED_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.+)")


@dataclass(frozen=True)
class Scenario:
    name: str
    active_players: frozenset[int]


SCENARIOS = (
    Scenario("neutral", frozenset()),
    Scenario("p1", frozenset((0,))),
    Scenario("p2", frozenset((1,))),
    Scenario("both", frozenset((0, 1))),
)


@dataclass(frozen=True)
class Config:
    renderer: str
    rate: int

    @property
    def name(self) -> str:
        return f"{self.renderer}-{self.rate}"

    @property
    def presents(self) -> int:
        return TICKS * (self.rate // 60)


CONFIGS = tuple(
    Config(renderer, rate)
    for renderer in ("gl", "webgpu")
    for rate in (60, 120)
)


@dataclass(frozen=True)
class Binding:
    tick: int
    player: int
    racer: int
    port: int
    held: int
    pressed: int
    released: int
    stick_x: int
    stick_y: int
    delta: int
    countdown: int


@dataclass(frozen=True)
class Pace:
    x: float
    y: float
    z: float
    checkpoint: int
    lap: int


@dataclass
class Result:
    output: str
    state_rows: list[str]
    input_rows: list[str]
    bindings: dict[tuple[int, int], Binding]
    pace: dict[int, dict[int, Pace]]
    sched: dict[str, int]


def scenario_script(base: str, scenario: Scenario) -> str:
    lines = [base.rstrip(), "", "# Focused production human-input window."]
    for player in sorted(scenario.active_players):
        port = player + 1
        lines.append(f"{DRIVE_START} A {DRIVE_END - DRIVE_START} P{port}")
        for start, end in LEFT_WINDOWS:
            lines.append(f"{start} LEFT {end - start} P{port}")
    return "\n".join(lines) + "\n"


def parse_fields(output: str) -> dict[str, int]:
    match = None
    for match in SCHED_RE.finditer(output):
        pass
    if match is None:
        raise RuntimeError("no [PRESENTSCHED-SUMMARY] row")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def parse_bindings(output: str) -> dict[tuple[int, int], Binding]:
    rows: dict[tuple[int, int], Binding] = {}
    for match in INPUT_RE.finditer(output):
        row = Binding(
            tick=int(match.group(1)), player=int(match.group(2)),
            racer=int(match.group(3)), port=int(match.group(4)),
            held=int(match.group(5), 16), pressed=int(match.group(6), 16),
            released=int(match.group(7), 16), stick_x=int(match.group(8)),
            stick_y=int(match.group(9)), delta=int(match.group(10)),
            countdown=int(match.group(11)),
        )
        rows[(row.tick, row.player)] = row
    return rows


def parse_pace(output: str) -> dict[int, dict[int, Pace]]:
    rows: dict[int, dict[int, Pace]] = {0: {}, 1: {}}
    for line in output.splitlines():
        match = PACE1_RE.search(line)
        if match is not None:
            rows[0][int(match.group(4))] = Pace(
                float(match.group(1)), float(match.group(2)),
                float(match.group(3)), int(match.group(5)),
                int(match.group(6)))
        match = PACE2_RE.search(line)
        if match is not None:
            rows[1][int(match.group(4))] = Pace(
                float(match.group(1)), float(match.group(2)),
                float(match.group(3)), int(match.group(5)),
                int(match.group(6)))
    return rows


def normalized_binding_rows(bindings: dict[tuple[int, int], Binding]) -> list[Binding]:
    return [bindings[key] for key in sorted(bindings)]


def run_arm(binary: Path, rom: Path, root: Path, script: Path,
            scenario: Scenario, config: Config, timeout: int,
            verbose: bool) -> Result:
    label = f"{scenario.name}-{config.name}"
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_INPUT_HASH="1",
        MDKR_RACER_INPUT_TRACE="1",
        MDKR_STATE_HASH="3",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_RATE=str(config.rate),
        MDKR_RENDERER=config.renderer,
        MDKR_SAVE_DIR=str(save_dir),
    )
    command = [
        str(binary), "--headless-frames", str(config.presents),
        "--window-size", "320x240", "--input-script", str(script),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    if "hud_init: hudPlayers=1 numViewports=2" not in output:
        raise RuntimeError(f"{label}: two-player HUD never loaded")
    state_rows = [line for line in output.splitlines()
                  if line.startswith("[SIMHASH]")]
    input_rows = [line for line in output.splitlines()
                  if line.startswith("[INPUTHASH]")]
    return Result(
        output=output, state_rows=state_rows, input_rows=input_rows,
        bindings=parse_bindings(output), pace=parse_pace(output),
        sched=parse_fields(output),
    )


def expected_held_and_stick(scenario: Scenario, player: int,
                            tick: int) -> tuple[int, int, int]:
    if player not in scenario.active_players:
        return 0, 0, 0
    held = 0x8000 if DRIVE_START <= tick < DRIVE_END else 0
    stick_x = 0
    if any(start <= tick < end for start, end in LEFT_WINDOWS):
        held |= 0x0200
        stick_x = CONSUMED_LEFT_STICK
    return held, stick_x, 0


def expected_input(scenario: Scenario, player: int,
                   tick: int) -> tuple[int, int, int, int, int]:
    held, stick_x, stick_y = expected_held_and_stick(
        scenario, player, tick)
    previous, _, _ = expected_held_and_stick(scenario, player, tick - 1)
    pressed = held & ~previous
    released = previous & ~held
    return held, pressed, released, stick_x, stick_y


def validate_binding(bindings: dict[tuple[int, int], Binding],
                     scenario: Scenario) -> list[str]:
    failures: list[str] = []
    for tick in range(DRIVE_START + TRACE_TICK_OFFSET, TICKS):
        for player in (0, 1):
            row = bindings.get((tick, player))
            if row is None:
                failures.append(
                    f"{scenario.name}: missing P{player + 1} binding at tick {tick}")
                return failures
            authored_tick = tick - TRACE_TICK_OFFSET
            held, pressed, released, stick_x, stick_y = expected_input(
                scenario, player, authored_tick)
            if row.player != player or row.port != player or row.racer != player:
                failures.append(
                    f"{scenario.name}: tick {tick} P{player + 1} mapped "
                    f"player/racer/port={row.player}/{row.racer}/{row.port}")
                return failures
            if (row.held, row.pressed, row.released,
                    row.stick_x, row.stick_y) != \
                    (held, pressed, released, stick_x, stick_y):
                failures.append(
                    f"{scenario.name}: tick {tick} P{player + 1} got "
                    f"held/pressed/released={row.held:04x}/{row.pressed:04x}/"
                    f"{row.released:04x} stick={row.stick_x},{row.stick_y}; "
                    f"expected {held:04x}/{pressed:04x}/{released:04x} "
                    f"{stick_x},{stick_y}")
                return failures
            if row.delta != 1:
                failures.append(
                    f"{scenario.name}: tick {tick} P{player + 1} delta="
                    f"{row.delta}, expected Enhanced fixed1")
                return failures
    return failures


def distance(a: Pace, b: Pace) -> float:
    return math.dist((a.x, a.y, a.z), (b.x, b.y, b.z))


def pace_metrics(rows: dict[int, Pace]) -> dict[str, float | int]:
    clocks = sorted(clock for clock in rows if clock > 0)
    if not clocks:
        return {"rows": 0, "path": 0.0, "displacement": 0.0,
                "max_step": 0.0, "checkpoint": -1, "lap": -1}
    path = 0.0
    max_step = 0.0
    for before, after in zip(clocks, clocks[1:]):
        if after != before + 1:
            continue
        step = distance(rows[before], rows[after])
        path += step
        max_step = max(max_step, step)
    first, last = rows[clocks[0]], rows[clocks[-1]]
    return {
        "rows": len(clocks), "path": path,
        "displacement": distance(first, last), "max_step": max_step,
        "checkpoint": max(row.checkpoint for row in rows.values()),
        "lap": max(row.lap for row in rows.values()),
    }


def validate_motion(result: Result, scenario: Scenario) -> tuple[list[str], str]:
    failures: list[str] = []
    metrics: dict[int, dict[str, float | int]] = {}
    for player in (0, 1):
        rows = result.pace[player]
        metrics[player] = pace_metrics(rows)
        if int(metrics[player]["rows"]) < 1000:
            failures.append(
                f"{scenario.name}: P{player + 1} has only "
                f"{metrics[player]['rows']} live race-clock rows")
        for clock, row in rows.items():
            if not all(math.isfinite(value) and abs(value) < 1.0e30
                       for value in (row.x, row.y, row.z)):
                failures.append(
                    f"{scenario.name}: P{player + 1} non-finite at clock {clock}")
                break
            if not Y_MIN <= row.y <= Y_MAX:
                failures.append(
                    f"{scenario.name}: P{player + 1} y={row.y:.1f} outside "
                    f"[{Y_MIN}, {Y_MAX}] at clock {clock}")
                break
        if float(metrics[player]["max_step"]) > MAX_STEP:
            failures.append(
                f"{scenario.name}: P{player + 1} max step "
                f"{metrics[player]['max_step']:.1f} > {MAX_STEP}")
        active = player in scenario.active_players
        if active:
            if float(metrics[player]["path"]) < ACTIVE_MIN_PATH:
                failures.append(
                    f"{scenario.name}: P{player + 1} path "
                    f"{metrics[player]['path']:.1f} < {ACTIVE_MIN_PATH}")
            if int(metrics[player]["checkpoint"]) < ACTIVE_MIN_CHECKPOINT:
                failures.append(
                    f"{scenario.name}: P{player + 1} checkpoint "
                    f"{metrics[player]['checkpoint']} < "
                    f"{ACTIVE_MIN_CHECKPOINT}")
        elif float(metrics[player]["path"]) > 100.0:
            failures.append(
                f"{scenario.name}: neutral P{player + 1} travelled "
                f"{metrics[player]['path']:.1f} units")
    note = (
        f"{scenario.name}: P1 path/cp/maxstep="
        f"{metrics[0]['path']:.1f}/{metrics[0]['checkpoint']}/"
        f"{metrics[0]['max_step']:.1f}; P2="
        f"{metrics[1]['path']:.1f}/{metrics[1]['checkpoint']}/"
        f"{metrics[1]['max_step']:.1f}")
    return failures, note


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--quick", action="store_true",
                        help="run only GL/60 for local calibration")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, BASE_SCRIPT):
        if not path.is_file():
            print(f"check_2p_human_binding: FAIL\n  - missing {path}")
            return 1

    configs = CONFIGS if not args.quick else (Config("gl", 60),)
    failures: list[str] = []
    notes: list[str] = []
    results: dict[tuple[str, str], Result] = {}
    with tempfile.TemporaryDirectory(prefix="mdkr-2p-human-binding-") as tmp:
        root = Path(tmp)
        base = BASE_SCRIPT.read_text(encoding="utf-8")
        scripts: dict[str, Path] = {}
        for scenario in SCENARIOS:
            script = root / f"{scenario.name}.txt"
            script.write_text(scenario_script(base, scenario), encoding="utf-8")
            scripts[scenario.name] = script

        for scenario in SCENARIOS:
            for config in configs:
                try:
                    result = run_arm(
                        binary, rom, root, scripts[scenario.name], scenario,
                        config, args.timeout, args.verbose)
                except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
                    failures.append(str(error))
                    continue
                results[(scenario.name, config.name)] = result

        canonical_name = configs[0].name
        for scenario in SCENARIOS:
            canonical = results.get((scenario.name, canonical_name))
            if canonical is None:
                continue
            failures.extend(validate_binding(canonical.bindings, scenario))
            motion_failures, note = validate_motion(canonical, scenario)
            failures.extend(motion_failures)
            notes.append(note)
            for key, expected in (("ticks", TICKS), ("simticks", TICKS),
                                  ("presents", configs[0].presents),
                                  ("presentrate", configs[0].rate),
                                  ("tickfields", 1), ("updatemin", 1),
                                  ("updatemax", 1)):
                if canonical.sched.get(key) != expected:
                    failures.append(
                        f"{scenario.name}: {key}={canonical.sched.get(key)}, "
                        f"expected {expected}")
            if len(canonical.state_rows) != TICKS:
                failures.append(
                    f"{scenario.name}: {len(canonical.state_rows)} state rows, "
                    f"expected {TICKS}")
            if len(canonical.input_rows) != TICKS:
                failures.append(
                    f"{scenario.name}: {len(canonical.input_rows)} input rows, "
                    f"expected {TICKS}")

            for config in configs[1:]:
                other = results.get((scenario.name, config.name))
                if other is None:
                    continue
                if other.state_rows != canonical.state_rows:
                    failures.append(
                        f"{scenario.name}: v3 state differs on {config.name}")
                if other.input_rows != canonical.input_rows:
                    failures.append(
                        f"{scenario.name}: consumed input differs on "
                        f"{config.name}")
                if normalized_binding_rows(other.bindings) != \
                        normalized_binding_rows(canonical.bindings):
                    failures.append(
                        f"{scenario.name}: racer binding differs on "
                        f"{config.name}")
                if other.pace != canonical.pace:
                    failures.append(
                        f"{scenario.name}: per-clock trajectory differs on "
                        f"{config.name}")
                for key, expected in (
                        ("ticks", TICKS), ("simticks", TICKS),
                        ("presents", config.presents), ("presentrate", config.rate),
                        ("tickfields", 1), ("updatemin", 1), ("updatemax", 1)):
                    if other.sched.get(key) != expected:
                        failures.append(
                            f"{scenario.name}/{config.name}: {key}="
                            f"{other.sched.get(key)}, expected {expected}")

        # Binding detector controls use the real P2/both observations. A
        # validator that accepted either mutation could not detect a
        # port-starvation or cross-wire failure.
        p2_result = results.get(("p2", canonical_name))
        both_result = results.get(("both", canonical_name))
        if p2_result is not None:
            dropped = {
                key: row for key, row in p2_result.bindings.items()
                if not (key[1] == 1 and
                        key[0] >= DRIVE_START + TRACE_TICK_OFFSET)
            }
            if not validate_binding(dropped, SCENARIOS[2]):
                failures.append("drop-P2 binding control was accepted")
        if both_result is not None:
            swapped = {
                key: (replace(row, port=1 - row.port)
                      if row.player in (0, 1) and
                      row.tick >= DRIVE_START + TRACE_TICK_OFFSET
                      else row)
                for key, row in both_result.bindings.items()
            }
            if not validate_binding(swapped, SCENARIOS[3]):
                failures.append("swapped-port binding control was accepted")

        # The neutral run must fail the active motion/progression contract for
        # both players. This is an executable sensitivity check, not a comment.
        neutral_result = results.get(("neutral", canonical_name))
        if neutral_result is not None:
            forced_active = Scenario("neutral-as-both", frozenset((0, 1)))
            control_failures, _ = validate_motion(neutral_result, forced_active)
            if not control_failures:
                failures.append("neutral motion control was accepted as both-active")

    if failures:
        print("check_2p_human_binding: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        for note in notes:
            print(f"  - observed: {note}")
        return 1

    print("check_2p_human_binding: PASS")
    for note in notes:
        print(f"  - {note}")
    if not args.quick:
        print(
            "  - all four scenarios v3/input/binding/trajectory-identical "
            "across GL/WebGPU and 60/120 Hz presentation; drop-P2, swapped-port, "
            "and neutral-motion controls rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
