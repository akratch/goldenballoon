#!/usr/bin/env python3
"""Presentation-rate invariance across pause, restart, and post-race teardown.

The broad content matrix proves fixed authority while races are running, but a
retained presentation packet is most dangerous when the game frees or reissues
the display-list/object arenas beneath it. This gate drives the three lifecycle
edges the original completion contract named explicitly:

* a 2P pause -> Select Track path that unloads without a following level_load;
* a production pause -> Restart Race path that reissues a race generation; and
* an Adventure loss through post-race results, lobby return, and hub return.

For each route, original-rate and production 60 Hz host pacing must produce
byte-identical v3 state, ordered gameplay events, consumed input, and temporary
PCM. The production arm deliberately requests public ``interpolate`` and must
perform real immutable replay across every retirement boundary. The presentation
matrix separately poisons the complete live arena on every replay; this gate
focuses its longer budget on unload/reissue coverage. A one-row state-hash
perturbation is a same-binary comparator control.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (completed_tick_conservation, DEFAULT_BUILD_DIR,
                           parse_last, resolve_binary)


ROOT = Path(__file__).resolve().parent.parent
HASH_VERSION = "3"

LOAD_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+)")
MENU_RE = re.compile(r"menu_init: menuId=(-?\d+)")

ADVENTURE_DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099;12:E5:E0"
)


@dataclass(frozen=True)
class Scenario:
    name: str
    clause: str
    script: Path
    ticks: int
    environment: dict[str, str]


@dataclass(frozen=True)
class Result:
    output: str
    state: list[str]
    events: list[str]
    inputs: list[str]
    audio_digest: str
    summary: dict[str, int]
    audio: dict[str, int]
    replay: dict[str, int]
    packet: dict[str, int]
    retained: dict[str, int]
    snapshot: dict[str, int]


SCENARIOS = (
    Scenario(
        "pause-quit",
        "2P pause and no-level-load teardown",
        ROOT / "tests" / "input_scripts" / "race_2p_quit_to_tracks.txt",
        4500,
        {"MDKR_LOAD_TRACK": "5"},
    ),
    Scenario(
        "pause-restart",
        "production pause-menu race restart",
        ROOT / "tests" / "input_scripts" / "race_pause_restart.txt",
        5200,
        {"MDKR_LOAD_TRACK": "5"},
    ),
    Scenario(
        "adventure-postrace",
        "post-race results, lobby return, hub return",
        ROOT / "tests" / "input_scripts" / "adventure_race_loop.txt",
        17000,
        {
            "MDKR_DRIVE_ROUTE": ADVENTURE_DRIVE_ROUTE,
            "MDKR_ADVENTURE_LOSS": "1",
        },
    ),
)


def stream(output: str, marker: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(marker)]


def run(binary: Path, rom: Path, root: Path, scenario: Scenario,
        policy: str | None, timeout: int, verbose: bool,
        extra: dict[str, str] | None = None,
        label_suffix: str | None = None) -> Result:
    label = f"{scenario.name}-{'original' if policy is None else policy}"
    if label_suffix:
        label += f"-{label_suffix}"
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    audio_path = run_dir / "audio.wav"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUDIO_DUMP=str(audio_path),
        MDKR_AUDIO_SERVICE_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_TRACE="1",
    )
    env.update(scenario.environment)
    if policy is not None:
        env["MDKR_PRESENT_RATE"] = policy
    if extra:
        env.update(extra)
    command = [
        str(binary), "--headless-ticks", str(scenario.ticks),
        "--input-script", str(scenario.script), "--rom", str(rom),
        "--window-size", "320x240",
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
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-4000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    if not audio_path.is_file() or audio_path.stat().st_size <= 44:
        raise RuntimeError(f"{label}: audio service produced no PCM capture")
    return Result(
        output=output,
        state=stream(output, "[SIMHASH]"),
        events=stream(output, "[EVENTHASH]"),
        inputs=stream(output, "[INPUTHASH]"),
        audio_digest=hashlib.sha256(audio_path.read_bytes()).hexdigest(),
        summary=parse_last(output, "PRESENTSCHED-SUMMARY"),
        audio=parse_last(output, "AUDIO-SERVICE"),
        replay=parse_last(output, "REPLAY-SUMMARY"),
        packet=parse_last(output, "PRESENT-PACKET"),
        retained=parse_last(output, "RETAINED-TASK"),
        snapshot=parse_last(output, "SNAPSHOT"),
    )


def coverage_failures(scenario: Scenario, result: Result,
                      label: str) -> list[str]:
    failures: list[str] = []
    loads = [
        (match.start(), *(int(value) for value in match.groups()))
        for match in LOAD_RE.finditer(result.output)
    ]
    menus = [
        (match.start(), int(match.group(1)))
        for match in MENU_RE.finditer(result.output)
    ]
    prefix = f"{scenario.name}-{label}"

    if scenario.name == "pause-quit":
        race = next((row for row in loads if row[1:3] == (5, 1)), None)
        returned = race is not None and any(
            position > race[0] and menu == 15 for position, menu in menus)
        if race is None:
            failures.append(f"{prefix}: never loaded Ancient Lake for 2P")
        elif not returned:
            failures.append(
                f"{prefix}: pause quit never reached Track Select after the race")
    elif scenario.name == "pause-restart":
        race_loads = [row for row in loads if row[1:3] == (5, 0)]
        if len(race_loads) < 2:
            failures.append(
                f"{prefix}: saw {len(race_loads)} one-player Ancient Lake "
                "loads; pause Restart Race did not create a second generation")
    else:
        wanted = [(0, 0), (12, 0), (5, 0), (12, 0), (0, 0)]
        cursor = 0
        matched: list[tuple[int, ...]] = []
        for row in loads:
            if cursor < len(wanted) and row[1:3] == wanted[cursor]:
                matched.append(row)
                cursor += 1
        if cursor != len(wanted):
            failures.append(
                f"{prefix}: Adventure lifecycle stopped at {cursor}/{len(wanted)} "
                f"loads in {wanted}; saw {[(row[1], row[2]) for row in loads]}")
        elif matched[-2][3] != 3 or matched[-1][3] != 2:
            failures.append(
                f"{prefix}: post-race returns used entrances "
                f"{matched[-2][3]}/{matched[-1][3]}, expected lobby/hub 3/2")
    return failures


def first_difference(reference: list[str], actual: list[str]) -> int | None:
    for index, pair in enumerate(zip(reference, actual)):
        if pair[0] != pair[1]:
            return index
    if len(reference) != len(actual):
        return min(len(reference), len(actual))
    return None


def validate_pair(scenario: Scenario, baseline: Result,
                  high: Result, poison: bool) -> list[str]:
    failures: list[str] = []
    prefix = scenario.name
    failures.extend(coverage_failures(scenario, baseline, "original"))
    failures.extend(coverage_failures(scenario, high, "60"))

    for name, left, right in (
            ("v3 state", baseline.state, high.state),
            ("ordered event", baseline.events, high.events),
            ("consumed input", baseline.inputs, high.inputs)):
        difference = first_difference(left, right)
        if difference is not None:
            failures.append(
                f"{prefix}: {name} stream diverged at tick {difference}")
        if len(left) != scenario.ticks or len(right) != scenario.ticks:
            failures.append(
                f"{prefix}: {name} lengths {len(left)}/{len(right)}, "
                f"expected {scenario.ticks}/{scenario.ticks}")
    if baseline.audio_digest != high.audio_digest:
        failures.append(f"{prefix}: temporary PCM differs at 60 Hz")

    for label, result, presents, kind, rate in (
            ("original", baseline, scenario.ticks, 0, 0),
            ("60", high, scenario.ticks * 2, 1, 60)):
        summary = result.summary
        conservation_error = completed_tick_conservation(
            summary, scenario.ticks, f"{prefix}-{label}")
        if conservation_error:
            failures.append(conservation_error)
        for key, expected in (
                ("presents", presents),
                ("entries", presents), ("presentkind", kind),
                ("presentrate", rate), ("updatebad", 0)):
            if summary.get(key) != expected:
                failures.append(
                    f"{prefix}-{label}: {key}={summary.get(key)}, expected {expected}")
        for key in ("multidue", "catchup", "skips", "rebases",
                    "blocked"):
            if summary.get(key, 0) != 0:
                failures.append(
                    f"{prefix}-{label}: {key}={summary.get(key)}, expected 0")
        audio = result.audio
        for key, expected in (
                ("fields", scenario.ticks * 2), ("due", scenario.ticks),
                ("serviced", scenario.ticks), ("pending", 0),
                ("retired", 0), ("rebases", 0),
                ("calls", scenario.ticks), ("dropped", 0)):
            if audio.get(key) != expected:
                failures.append(
                    f"{prefix}-{label}: audio {key}={audio.get(key)}, "
                    f"expected {expected}")

    if (high.summary.get("interp", 0) <= 0 or
            high.replay.get("walks", 0) <= 0 or
            high.replay.get("walks") != high.summary.get("interp")):
        failures.append(
            f"{prefix}: public 60 Hz smoothing did not produce matching "
            "replay walks and interpolated images")
    expected_surface = (high.summary.get("realendpoints", 0) +
                        high.summary.get("interp", 0))
    if high.summary.get("surfaceupdates") != expected_surface:
        failures.append(
            f"{prefix}: production surfaceupdates="
            f"{high.summary.get('surfaceupdates')}, expected "
            f"real+interpolated={expected_surface}")
    if high.summary.get("replayendpoints", -1) != 0:
        failures.append(
            f"{prefix}: ordinary production redrew "
            f"{high.summary.get('replayendpoints')} endpoints")
    expected_poison = high.replay.get("walks", 0) if poison else 0
    if (high.retained.get("captures", 0) <= 0 or
            high.retained.get("captures") != high.retained.get("begins") or
            high.retained.get("failures", -1) != 0 or
            high.retained.get("rejects", -1) != 0 or
            high.retained.get("arenaResolve", 0) <= 0 or
            high.retained.get("externalResolve", 0) <= 0 or
            high.retained.get("livePoison", -1) != expected_poison or
            high.packet.get("futurecaptures", 0) <= 0 or
            high.packet.get("futurefailures", -1) != 0):
        failures.append(
            f"{prefix}: retained/future publication contract failed "
            f"(retained={high.retained}, poison={expected_poison})")
    for owner, fields in (
            ("replay", ("freezefail", "restorefail", "mtxreject")),
            ("packet", ("freezefail", "deformcollision", "effectcollision")),
            ("snapshot", ("overflows",))):
        values = getattr(high, owner)
        for key in fields:
            if values.get(key, 0) != 0:
                failures.append(
                    f"{prefix}: {owner} {key}={values.get(key)}, expected 0")
    return failures


def validate_control(reference: Result, control: Result) -> list[str]:
    failures: list[str] = []
    differences = [
        index for index, pair in enumerate(zip(reference.state, control.state))
        if pair[0] != pair[1]
    ]
    if len(reference.state) != len(control.state):
        failures.append("state perturbation changed the stream length")
    elif len(differences) != 1:
        failures.append(
            f"state perturbation changed {len(differences)} rows, expected exactly 1")
    if reference.events != control.events:
        failures.append("state-only perturbation changed the event stream")
    if reference.inputs != control.inputs:
        failures.append("state-only perturbation changed the input stream")
    if reference.audio_digest != control.audio_digest:
        failures.append("state-only perturbation changed temporary PCM")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--only", default=None,
                        help="comma-separated scenario names (iteration only)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    scenarios = list(SCENARIOS)
    if args.only:
        wanted = {item.strip() for item in args.only.split(",")}
        scenarios = [scenario for scenario in scenarios if scenario.name in wanted]
    for path in (binary, rom, *(scenario.script for scenario in scenarios)):
        if not path.is_file():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    if not scenarios:
        print("FAIL: --only selected no scenarios", file=sys.stderr)
        return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-present-lifecycle-") as temp:
        root = Path(temp)
        high_results: dict[str, Result] = {}
        try:
            for scenario in scenarios:
                baseline = run(binary, rom, root, scenario, None,
                               args.timeout, args.verbose)
                production = run(
                    binary, rom, root, scenario, "60", args.timeout,
                    args.verbose,
                    {"MDKR_PRESENT_SMOOTHING": "interpolate"},
                    "production")
                high_results[scenario.name] = production
                failures.extend(validate_pair(
                    scenario, baseline, production, False))
                notes.append(
                    f"{scenario.name}: {scenario.ticks} fixed ticks, "
                    f"production {production.replay.get('walks')} immutable "
                    "replays across the lifecycle boundary")
            if "pause-quit" in high_results:
                scenario = next(item for item in scenarios
                                if item.name == "pause-quit")
                control = run(
                    binary, rom, root, scenario, "60", args.timeout,
                    args.verbose,
                    {"MDKR_PRESENT_SMOOTHING": "interpolate",
                     "MDKR_TEST_HASH_PERTURB": "global:4000"},
                    "hash-control",
                )
                failures.extend(validate_control(
                    high_results["pause-quit"], control))
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(str(error))

    if failures:
        print("check_presentation_lifecycle: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    covered = ", ".join(scenario.name for scenario in scenarios)
    print("check_presentation_lifecycle: PASS -- "
          f"{covered} preserve state/event/input/PCM under 60 Hz presentation")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
