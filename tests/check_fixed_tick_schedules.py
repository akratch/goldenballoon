#!/usr/bin/env python3
"""Fixed-step authority must survive lateness and suspension rebases.

This is the application-level complement to ``host_frame_driver``'s rational
unit test. It runs the same scripted route with synthetic host opportunities
representing 2, 3, 4, 5, and 6 source fields, plus deterministic suspension
rebases. Every scheduled game pass must consume one two-field ticket and emit
the same v3 authority, ordered gameplay-event, and consumed-input streams.
Intermediate catch-up lists must be elided so presents plus elisions equal the
tick budget without discarding simulation work. The independently due audio
service must produce byte-identical temporary PCM and equal sample time across
the same schedules; the captures never leave the test's temporary directory.
``MDKR_VI_PACE=off`` is the state sensitivity control: it deliberately lies to
the game about the ticket width and must be detected. A trace-only phantom
event is the complementary event-stream control and must change exactly one
event row without changing v3 state; an input-trace perturbation provides the
same independent control for consumed pad samples. A one-quantum PCM timing
perturbation must change the audio capture without changing state, events, or
consumed input.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
EVENT_SUMMARY_RE = re.compile(r"\[EVENTHASH-SUMMARY\] (.*)")
INPUT_SUMMARY_RE = re.compile(r"\[INPUTHASH-SUMMARY\] (.*)")
INPUTQ_SUMMARY_RE = re.compile(r"\[INPUTQ-SUMMARY\] (.*)")
AUDIO_SUMMARY_RE = re.compile(r"\[AUDIO-SERVICE\] (.*)")
TICKS = 1800


def parse_summary(output: str) -> dict[str, int]:
    matches = list(SUMMARY_RE.finditer(output))
    if not matches:
        raise RuntimeError("missing [PRESENTSCHED-SUMMARY]")
    fields: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = int(value)
    return fields


def parse_event_summary(output: str) -> dict[str, int]:
    matches = list(EVENT_SUMMARY_RE.finditer(output))
    if not matches:
        raise RuntimeError("missing [EVENTHASH-SUMMARY]")
    fields: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = int(value)
    return fields


def parse_named_summary(
        output: str, pattern: re.Pattern[str], name: str) -> dict[str, int]:
    matches = list(pattern.finditer(output))
    if not matches:
        raise RuntimeError(f"missing [{name}]")
    fields: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = int(value)
    return fields


def hashes(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith("[SIMHASH]")]


def event_hashes(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith("[EVENTHASH]")]


def input_hashes(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith("[INPUTHASH]")]


def run(binary: Path, rom: Path, run_root: Path, label: str,
        extra_env: dict[str, str], timeout: int, verbose: bool
        ) -> tuple[
            list[str], list[str], dict[str, int], dict[str, int],
            list[str], dict[str, int], dict[str, int], dict[str, int], str]:
    save_dir = run_root / label / "save"
    save_dir.mkdir(parents=True)
    audio_path = run_root / label / "schedule-audio.wav"
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
        MDKR_LOAD_TRACK="5",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH="3",
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-ticks", str(TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_root / label, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    if not audio_path.is_file() or audio_path.stat().st_size <= 44:
        raise RuntimeError(f"{label}: audio service produced no PCM capture")
    digest = hashlib.sha256(audio_path.read_bytes()).hexdigest()
    return (
        hashes(output), event_hashes(output), parse_summary(output),
        parse_event_summary(output), input_hashes(output),
        parse_named_summary(output, INPUT_SUMMARY_RE, "INPUTHASH-SUMMARY"),
        parse_named_summary(output, INPUTQ_SUMMARY_RE, "INPUTQ-SUMMARY"),
        parse_named_summary(output, AUDIO_SUMMARY_RE, "AUDIO-SERVICE"),
        digest,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    notes: list[str] = []
    arms = {
        "original": {},
        "late-3": {"MDKR_SYNTH_FIELDS": "3"},
        "late-4": {"MDKR_SYNTH_FIELDS": "4"},
        "late-5": {"MDKR_SYNTH_FIELDS": "5"},
        "late-6": {"MDKR_SYNTH_FIELDS": "6"},
        "rebase": {
            "MDKR_SYNTH_FIELDS": "6",
            "MDKR_TEST_PACE_REBASE_EVERY": "17",
        },
    }
    with tempfile.TemporaryDirectory(prefix="mdkr-fixed-tick-") as temp:
        root = Path(temp)
        results: dict[
            str, tuple[
                list[str], list[str], dict[str, int], dict[str, int],
                list[str], dict[str, int], dict[str, int], dict[str, int], str]
        ] = {}
        try:
            for label, arm_env in arms.items():
                results[label] = run(
                    binary, rom, root, label, arm_env,
                    args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_fixed_tick_schedules: FAIL\n  - {error}")
            return 1

        baseline = results["original"][0]
        baseline_events = results["original"][1]
        baseline_inputs = results["original"][4]
        baseline_audio = results["original"][8]
        if len(baseline) != TICKS:
            failures.append(
                f"original emitted {len(baseline)} hashes, expected {TICKS}")
        if len(baseline_events) != TICKS:
            failures.append(
                f"original emitted {len(baseline_events)} event rows, "
                f"expected {TICKS}")
        if len(baseline_inputs) != TICKS:
            failures.append(
                f"original emitted {len(baseline_inputs)} input rows, "
                f"expected {TICKS}")
        for label, result in results.items():
            (stream, events, summary, event_summary, inputs,
             input_summary, inputq_summary, audio_summary,
             audio_digest) = result
            if stream != baseline:
                difference = next(
                    (index for index, pair in enumerate(zip(baseline, stream))
                     if pair[0] != pair[1]),
                    min(len(baseline), len(stream)),
                )
                failures.append(
                    f"{label}: v3 stream diverged at tick {difference}")
            if events != baseline_events:
                difference = next(
                    (index for index, pair in
                     enumerate(zip(baseline_events, events))
                     if pair[0] != pair[1]),
                    min(len(baseline_events), len(events)),
                )
                failures.append(
                    f"{label}: event stream diverged at tick {difference}")
            if inputs != baseline_inputs:
                difference = next(
                    (index for index, pair in
                     enumerate(zip(baseline_inputs, inputs))
                     if pair[0] != pair[1]),
                    min(len(baseline_inputs), len(inputs)),
                )
                failures.append(
                    f"{label}: input-consumption stream diverged at tick "
                    f"{difference}")
            if audio_digest != baseline_audio:
                failures.append(
                    f"{label}: synthesized PCM diverged from original schedule")
            if event_summary.get("ticks") != TICKS:
                failures.append(
                    f"{label}: event ticks={event_summary.get('ticks')}, "
                    f"expected {TICKS}")
            if input_summary.get("ticks") != TICKS:
                failures.append(
                    f"{label}: input ticks={input_summary.get('ticks')}, "
                    f"expected {TICKS}")
            if inputq_summary.get("ticks") != TICKS:
                failures.append(
                    f"{label}: queue ticks={inputq_summary.get('ticks')}, "
                    f"expected {TICKS}")
            for key in ("overflow", "reordered", "pending"):
                if inputq_summary.get(key) != 0:
                    failures.append(
                        f"{label}: input queue {key}="
                        f"{inputq_summary.get(key)}, expected 0")
            for key, expected in (
                    ("fields", TICKS * 2), ("due", TICKS),
                    ("serviced", TICKS - 1), ("pending", 1),
                    ("retired", 0), ("calls", TICKS - 1),
                    ("idle", 0), ("notready", 0), ("dropped", 0),
                    ("quantumfields", 2)):
                if audio_summary.get(key) != expected:
                    failures.append(
                        f"{label}: audio {key}={audio_summary.get(key)}, "
                        f"expected {expected}")
            if audio_summary.get("samples", 0) <= 0:
                failures.append(f"{label}: audio service produced no samples")
            if audio_summary.get("samples") != results["original"][7].get(
                    "samples"):
                failures.append(
                    f"{label}: audio samples={audio_summary.get('samples')} "
                    "differ from original schedule")
            if audio_summary.get("rebases") != summary.get("rebases"):
                failures.append(
                    f"{label}: audio rebases={audio_summary.get('rebases')} "
                    f"but scheduler rebases={summary.get('rebases')}")
            audio_max_pending = audio_summary.get("maxpending", 0)
            if audio_max_pending < 1 or audio_max_pending > 30:
                failures.append(
                    f"{label}: audio maxpending={audio_max_pending}, "
                    "expected 1..30")
            for key in ("ticks", "simticks", "issued"):
                if summary.get(key) != TICKS:
                    failures.append(
                        f"{label}: {key}={summary.get(key)}, expected {TICKS}")
            if summary.get("presents", 0) + summary.get("elided", 0) != TICKS:
                failures.append(
                    f"{label}: presents+elided="
                    f"{summary.get('presents')}+{summary.get('elided')}, "
                    f"expected {TICKS}")
            if summary.get("presents") != summary.get("entries"):
                failures.append(
                    f"{label}: presents={summary.get('presents')} but host "
                    f"entries={summary.get('entries')}")
            expected_updates = TICKS - 1  # original boot bootstrap is separate
            if summary.get("updates") != expected_updates:
                failures.append(
                    f"{label}: updates={summary.get('updates')}, "
                    f"expected {expected_updates}")
            for key, expected in (
                    ("pending", 0), ("blocked", 0), ("updatebad", 0),
                    ("updatemin", 2), ("updatemax", 2),
                    ("tickfields", 2)):
                if summary.get(key) != expected:
                    failures.append(
                        f"{label}: {key}={summary.get(key)}, expected {expected}")
            max_pending = summary.get("maxpending", 0)
            if max_pending < 1 or max_pending > 30:
                failures.append(
                    f"{label}: maxpending={max_pending}, expected 1..30")
            notes.append(
                f"{label}: entries={summary.get('entries')} "
                f"multidue={summary.get('multidue')} "
                f"catchup={summary.get('catchup')} "
                f"rebases={summary.get('rebases')}")

        for label in ("late-3", "late-4", "late-5", "late-6"):
            summary = results[label][2]
            if summary.get("multidue", 0) == 0 or summary.get("catchup", 0) == 0:
                failures.append(f"{label}: did not exercise multi-tick catch-up")
            if summary.get("maxpending", 0) <= 1:
                failures.append(f"{label}: never retained multi-ticket debt")
            if summary.get("elided", 0) == 0:
                failures.append(f"{label}: catch-up renders were not elided")
            if summary.get("elided") != summary.get("catchup"):
                failures.append(
                    f"{label}: elided={summary.get('elided')} does not match "
                    f"catchup={summary.get('catchup')}")
        if results["original"][2].get("elided") != 0:
            failures.append("original: render elision activated without debt")
        if results["rebase"][2].get("rebases", 0) == 0:
            failures.append("rebase: deterministic suspension seam never fired")

        event_coverage = results["original"][3]
        for family in ("sound", "music", "transition", "spawn", "despawn",
                       "level", "context"):
            if event_coverage.get(family, 0) == 0:
                failures.append(
                    f"event route did not exercise the {family} hook family")
        input_coverage = results["original"][5]
        for family in ("active", "pressed", "released"):
            if input_coverage.get(family, 0) == 0:
                failures.append(
                    f"input route did not exercise {family} samples")

        try:
            control_result = run(
                binary, rom, root, "wrong-rate-control",
                {"MDKR_VI_PACE": "off"}, args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_fixed_tick_schedules: FAIL\n  - {error}")
            return 1
        control_stream = control_result[0]
        control_summary = control_result[2]
        if control_stream == baseline:
            failures.append(
                "sensitivity control stayed v3-identical after forcing "
                "one-field game updates")
        if control_summary.get("updatebad", 0) == 0:
            failures.append("sensitivity control recorded no bad updateRate")
        notes.append(
            "control: one-field diagnostic diverged and update-rate census "
            f"caught {control_summary.get('updatebad')} bad passes")

        try:
            event_result = run(
                binary, rom, root, "event-control",
                {"MDKR_TEST_EVENT_PERTURB": "900"},
                args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_fixed_tick_schedules: FAIL\n  - {error}")
            return 1
        perturbed_state = event_result[0]
        perturbed_events = event_result[1]
        if perturbed_state != baseline:
            failures.append(
                "phantom event control changed authoritative v3 state")
        changed_event_rows = sum(
            left != right for left, right in
            zip(baseline_events, perturbed_events))
        changed_event_rows += abs(
            len(baseline_events) - len(perturbed_events))
        if changed_event_rows != 1:
            failures.append(
                "phantom event control changed "
                f"{changed_event_rows} rows, expected exactly 1")
        notes.append(
            "control: one trace-only phantom event changed exactly one "
            "event row and zero v3 rows")

        try:
            input_result = run(
                binary, rom, root, "input-control",
                {"MDKR_TEST_INPUT_PERTURB": "900"},
                args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_fixed_tick_schedules: FAIL\n  - {error}")
            return 1
        if input_result[0] != baseline or input_result[1] != baseline_events:
            failures.append(
                "input trace control changed state or gameplay events")
        changed_input_rows = sum(
            left != right for left, right in
            zip(baseline_inputs, input_result[4]))
        changed_input_rows += abs(
            len(baseline_inputs) - len(input_result[4]))
        if changed_input_rows != 1:
            failures.append(
                "input trace control changed "
                f"{changed_input_rows} rows, expected exactly 1")
        notes.append(
            "control: one trace-only input perturbation changed exactly one "
            "input row and zero state/event rows")

        try:
            audio_result = run(
                binary, rom, root, "audio-control",
                {"MDKR_TEST_AUDIO_PERTURB": "900"},
                args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_fixed_tick_schedules: FAIL\n  - {error}")
            return 1
        if (audio_result[0] != baseline or
                audio_result[1] != baseline_events or
                audio_result[4] != baseline_inputs):
            failures.append(
                "audio timing control changed state, gameplay events, or input")
        if audio_result[8] == baseline_audio:
            failures.append(
                "audio timing control left the synthesized PCM unchanged")
        if audio_result[7].get("samples") == results["original"][7].get(
                "samples"):
            failures.append(
                "audio timing control did not change produced sample time")
        notes.append(
            "control: one PCM timing perturbation changed audio output and "
            "zero state/event/input rows")

    if failures:
        print("check_fixed_tick_schedules: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "check_fixed_tick_schedules: PASS — v3, ordered events, consumed input, "
        "and PCM identical across fixed-ticket lateness and suspension schedules")
    for note in notes:
        print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
