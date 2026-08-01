#!/usr/bin/env python3
"""Exact arbitrary-rate host pacing must remain gameplay- and replay-neutral.

Runs the same fixed-tick route at original, common numeric caps, and the
deterministic headless stand-in for uncapped presentation. Original cadence is
kept on the GL oracle lane, while Enhanced cadence is forced through WebGPU at
original/30/45/60/120/uncapped. Every arm must produce the exact rational
number of host opportunities while keeping the v3 authority, ordered gameplay-
event, consumed-input, and temporary PCM streams byte-identical. Production
1.0.1 must commit only complete authored images: every numeric/uncapped arm
requires zero interpolation, zero delayed walks, and zero replayed endpoints.
A PAL arm proves that 60 Hz need not be rounded onto the 50 Hz field grid.

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

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TICKS = 600
HASH_VERSION = "3"
UNCAPPED_SYNTHETIC_RATE = 1000

SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
AUDIO_RE = re.compile(r"\[AUDIO-SERVICE\] (.*)")
REPLAY_RE = re.compile(r"\[REPLAY-SUMMARY\] (.*)")
PACKET_RE = re.compile(r"\[PRESENT-PACKET\] (.*)")


@dataclass(frozen=True)
class Result:
    state: list[str]
    events: list[str]
    inputs: list[str]
    audio_digest: str
    summary: dict[str, int]
    audio: dict[str, int]
    replay: dict[str, int]
    packet: dict[str, int]


def parse_last(output: str, pattern: re.Pattern[str], name: str) -> dict[str, int]:
    matches = list(pattern.finditer(output))
    if not matches:
        raise RuntimeError(f"missing [{name}] summary")
    fields: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def stream(output: str, marker: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(marker)]


def find_pal_rom(directory: Path) -> Path | None:
    if not directory.is_dir():
        return None
    for path in sorted(directory.iterdir()):
        name = path.name.lower()
        if ("europe" in name or "pal" in name) and name.endswith(".z64"):
            return path
    return None


def run(binary: Path, rom: Path, root: Path, label: str,
        policy: str | None, timeout: int, verbose: bool,
        *, cadence: str = "original", renderer: str = "gl",
        extra_env: dict[str, str] | None = None) -> Result:
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
        MDKR_LOAD_TRACK="5",
        MDKR_PRESENT_SCHED_TRACE="1",
        # Raw public request deliberately bypasses the schema. Production must
        # still keep delayed replay quarantined without the explicit test seam.
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_RENDERER=renderer,
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_SIMULATION_CADENCE=cadence,
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_SYNTH_FIELDS="1" if cadence == "enhanced" else "2",
    )
    if renderer == "webgpu":
        # A hidden native Metal window intentionally has no drawable. This gate
        # verifies actual surface commits, so use the existing compositor-visible
        # headless lifecycle seam rather than confusing offscreen completion with
        # a presented image.
        env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
    if extra_env is not None:
        env.update(extra_env)
    if policy is not None:
        env["MDKR_PRESENT_RATE"] = policy
    command = [
        str(binary), "--headless-ticks", str(TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        setting = policy if policy is not None else "original-policy"
        print(f"$ ({label}) cadence={cadence} renderer={renderer} "
              f"MDKR_PRESENT_RATE={setting} {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    if not audio_path.is_file() or audio_path.stat().st_size <= 44:
        raise RuntimeError(f"{label}: audio service produced no PCM capture")
    return Result(
        stream(output, "[SIMHASH]"),
        stream(output, "[EVENTHASH]"),
        stream(output, "[INPUTHASH]"),
        hashlib.sha256(audio_path.read_bytes()).hexdigest(),
        parse_last(output, SUMMARY_RE, "PRESENTSCHED-SUMMARY"),
        parse_last(output, AUDIO_RE, "AUDIO-SERVICE"),
        parse_last(output, REPLAY_RE, "REPLAY-SUMMARY"),
        parse_last(output, PACKET_RE, "PRESENT-PACKET"),
    )


def expected_presents(rate: int, tick_rate: int) -> int:
    numerator = TICKS * rate
    if numerator % tick_rate != 0:
        raise AssertionError("test tick budget must end on an exact rate boundary")
    return numerator // tick_rate


def compare_arm(label: str, result: Result, baseline: Result,
                rate: int, tick_rate: int, policy_kind: int,
                policy_rate: int, tick_fields: int = 2,
                exact_surface: bool = True) -> list[str]:
    failures: list[str] = []
    expected = expected_presents(rate, tick_rate)
    for name, actual, reference in (
            ("v3 state", result.state, baseline.state),
            ("ordered event", result.events, baseline.events),
            ("consumed input", result.inputs, baseline.inputs)):
        if actual != reference:
            difference = next(
                (index for index, pair in enumerate(zip(reference, actual))
                 if pair[0] != pair[1]),
                min(len(reference), len(actual)),
            )
            failures.append(f"{label}: {name} stream diverged at tick {difference}")
    if result.audio_digest != baseline.audio_digest:
        failures.append(f"{label}: PCM capture differs from original")

    summary = result.summary
    for key in ("ticks", "simticks", "issued"):
        if summary.get(key) != TICKS:
            failures.append(f"{label}: {key}={summary.get(key)}, expected {TICKS}")
    expected_catchup = max(0, TICKS - expected)
    for key in ("pending", "lag", "skips", "rebases", "blocked",
                "updatebad"):
        if summary.get(key, 0) != 0:
            failures.append(f"{label}: {key}={summary.get(key)}, expected 0")
    expected_lead = 1 if expected_catchup != 0 else 0
    if summary.get("lead", 0) != expected_lead:
        failures.append(
            f"{label}: lead={summary.get('lead')}, expected {expected_lead}")
    for key in ("multidue", "catchup"):
        if summary.get(key, 0) != expected_catchup:
            failures.append(
                f"{label}: {key}={summary.get(key)}, expected "
                f"{expected_catchup}")
    for key, value in (("presents", expected), ("entries", expected),
                       ("fieldhz", 50 if tick_rate == 25 else 60),
                       ("tickfields", tick_fields),
                       ("presentkind", policy_kind),
                       ("presentrate", policy_rate)):
        if summary.get(key) != value:
            failures.append(
                f"{label}: {key}={summary.get(key)}, expected {value}")
    if summary.get("interp", -1) != 0:
        failures.append(
            f"{label}: production interp={summary.get('interp')}, expected 0")
    expected_surface_updates = min(TICKS, expected)
    expected_real_endpoints = (
        0 if policy_kind == 0 else expected_surface_updates)
    surface_updates = summary.get("surfaceupdates", -1)
    if exact_surface:
        if surface_updates != expected_surface_updates:
            failures.append(
                f"{label}: surfaceupdates={surface_updates}, expected "
                f"{expected_surface_updates}")
    elif not (0 < surface_updates <= expected_surface_updates):
        failures.append(
            f"{label}: WebGPU surfaceupdates={surface_updates}, expected "
            f"1..{expected_surface_updates}; compositor availability may "
            "hold authored images but must never create extra ones")
    for key, value in (
            ("realendpoints", expected_real_endpoints),
            ("replayendpoints", 0)):
        if summary.get(key) != value:
            failures.append(
                f"{label}: {key}={summary.get(key)}, expected {value}")

    audio = result.audio
    due = TICKS * tick_fields // 2
    serviced = due - 1
    service_calls = TICKS - 1
    for key, value in (("fields", TICKS * tick_fields), ("due", due),
                       ("serviced", serviced), ("pending", 1),
                       ("retired", 0), ("rebases", 0),
                       ("calls", service_calls),
                       ("idle", service_calls - serviced),
                       ("notready", 0), ("dropped", 0),
                       ("quantumfields", 2)):
        if audio.get(key) != value:
            failures.append(
                f"{label}: audio {key}={audio.get(key)}, expected {value}")
    if audio.get("samples") != baseline.audio.get("samples"):
        failures.append(
            f"{label}: audio samples={audio.get('samples')}, expected "
            f"{baseline.audio.get('samples')}")

    replay = result.replay
    packet = result.packet
    if replay.get("walks", -1) != 0:
        failures.append(
            f"{label}: production replay walks={replay.get('walks')}, expected 0")
    for key in ("freezefail", "restorefail"):
        if replay.get(key, 0) != 0:
            failures.append(f"{label}: replay {key}={replay.get(key)}")
    for key in ("freezefail", "deformcollision", "effectcollision"):
        if packet.get(key, 0) != 0:
            failures.append(f"{label}: packet {key}={packet.get(key)}")
    return failures


def compare_display_arm(label: str, result: Result,
                        baseline: Result,
                        exact_surface: bool = True) -> list[str]:
    """Validate native display pacing without assuming the monitor refresh."""
    presents = result.summary.get("presents", 0)
    numerator = presents * 30
    if presents < TICKS or numerator % TICKS != 0:
        return [
            f"{label}: display opportunities={presents} do not resolve to an "
            "integral supported refresh at or above the authored 30 Hz cadence"
        ]
    rate = numerator // TICKS
    if rate < 30 or rate > 1000:
        return [f"{label}: resolved display rate {rate} is outside 30..1000 Hz"]
    return compare_arm(
        label, result, baseline, rate, 30, 2, 0,
        exact_surface=exact_surface)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--roms", default="build/roms")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    pal_rom = find_pal_rom(Path(os.path.abspath(args.roms)))
    for path in (binary, rom, SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    if pal_rom is None:
        print("check_arbitrary_presentation_rates: FAIL\n"
              f"  - no PAL v80 release found below {args.roms}; the 50 Hz "
              "sub-field proof is mandatory")
        return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-arbitrary-rate-") as temp:
        root = Path(temp)
        try:
            ntsc_base = run(binary, rom, root, "ntsc-original", None,
                            args.timeout, args.verbose)
            if not (len(ntsc_base.state) == len(ntsc_base.events) ==
                    len(ntsc_base.inputs) == TICKS):
                failures.append(
                    "ntsc-original: state/event/input stream lengths are "
                    f"{len(ntsc_base.state)}/{len(ntsc_base.events)}/"
                    f"{len(ntsc_base.inputs)}, expected {TICKS}/{TICKS}/{TICKS}")
            failures.extend(compare_arm(
                "ntsc-original", ntsc_base, ntsc_base, 30, 30, 0, 0))

            gl_display = run(binary, rom, root, "ntsc-display", "display",
                             args.timeout, args.verbose)
            failures.extend(compare_display_arm(
                "ntsc-display", gl_display, ntsc_base))
            notes.append(
                f"ntsc-display: {gl_display.summary.get('presents')} host "
                f"opportunities / {gl_display.summary.get('surfaceupdates')} "
                "authored surface updates")

            webgpu_display = run(
                binary, rom, root, "ntsc-webgpu-display", "display",
                args.timeout, args.verbose, renderer="webgpu")
            failures.extend(compare_display_arm(
                "ntsc-webgpu-display", webgpu_display, ntsc_base,
                exact_surface=False))
            notes.append(
                f"ntsc-webgpu-display: "
                f"{webgpu_display.summary.get('presents')} host opportunities "
                f"/ {webgpu_display.summary.get('surfaceupdates')} authored "
                "surface updates")

            for label, policy, rate in (
                    ("ntsc-30", "30", 30),
                    ("ntsc-60", "60", 60),
                    ("ntsc-120", "120", 120),
                    ("ntsc-144", "144", 144),
                    ("ntsc-165", "165", 165),
                    ("ntsc-240", "240", 240),
                    ("ntsc-uncapped", "uncapped", UNCAPPED_SYNTHETIC_RATE)):
                result = run(binary, rom, root, label, policy,
                             args.timeout, args.verbose)
                kind = 3 if policy == "uncapped" else 1
                published_rate = 0 if policy == "uncapped" else rate
                failures.extend(compare_arm(
                    label, result, ntsc_base, rate, 30, kind, published_rate))
                notes.append(
                    f"{label}: {result.summary.get('presents')} presents / "
                    f"{result.summary.get('ticks')} fixed ticks")

            enhanced_base = run(
                binary, rom, root, "enhanced-webgpu-original", None,
                args.timeout, args.verbose, cadence="enhanced",
                renderer="webgpu")
            if not (len(enhanced_base.state) == len(enhanced_base.events) ==
                    len(enhanced_base.inputs) == TICKS):
                failures.append(
                    "enhanced-webgpu-original: state/event/input stream "
                    f"lengths are {len(enhanced_base.state)}/"
                    f"{len(enhanced_base.events)}/"
                    f"{len(enhanced_base.inputs)}, expected "
                    f"{TICKS}/{TICKS}/{TICKS}")
            failures.extend(compare_arm(
                "enhanced-webgpu-original", enhanced_base, enhanced_base,
                60, 60, 0, 0, 1, exact_surface=False))
            notes.append(
                "enhanced-webgpu-original: "
                f"{enhanced_base.summary.get('presents')} presents / "
                f"{enhanced_base.summary.get('ticks')} fixed ticks")

            for label, policy, rate in (
                    ("enhanced-webgpu-30", "30", 30),
                    ("enhanced-webgpu-45", "45", 45),
                    ("enhanced-webgpu-60", "60", 60),
                    ("enhanced-webgpu-120", "120", 120),
                    ("enhanced-webgpu-uncapped", "uncapped",
                     UNCAPPED_SYNTHETIC_RATE)):
                result = run(
                    binary, rom, root, label, policy,
                    args.timeout, args.verbose, cadence="enhanced",
                    renderer="webgpu")
                kind = 3 if policy == "uncapped" else 1
                published_rate = 0 if policy == "uncapped" else rate
                failures.extend(compare_arm(
                    label, result, enhanced_base, rate, 60, kind,
                    published_rate, 1, exact_surface=False))
                notes.append(
                    f"{label}: {result.summary.get('presents')} presents / "
                    f"{result.summary.get('ticks')} fixed ticks")

            enhanced_audio_control = run(
                binary, rom, root, "enhanced-webgpu-audio-control", "60",
                args.timeout, args.verbose, cadence="enhanced",
                renderer="webgpu",
                extra_env={"MDKR_TEST_AUDIO_PERTURB": "150"})
            for name, actual, reference in (
                    ("v3 state", enhanced_audio_control.state,
                     enhanced_base.state),
                    ("ordered event", enhanced_audio_control.events,
                     enhanced_base.events),
                    ("consumed input", enhanced_audio_control.inputs,
                     enhanced_base.inputs)):
                if actual != reference:
                    failures.append(
                        "enhanced-webgpu-audio-control: trace-only audio "
                        f"control changed {name}")
            if enhanced_audio_control.audio_digest == enhanced_base.audio_digest:
                failures.append(
                    "enhanced-webgpu-audio-control: PCM perturbation did not "
                    "change the capture")
            if enhanced_audio_control.audio.get("samples") == \
                    enhanced_base.audio.get("samples"):
                failures.append(
                    "enhanced-webgpu-audio-control: PCM perturbation did not "
                    "change produced sample time")
            notes.append(
                "enhanced WebGPU audio negative control changed PCM/sample "
                "time and zero state/event/input rows")

            pal_base = run(binary, pal_rom, root, "pal-original", None,
                           args.timeout, args.verbose)
            if not (len(pal_base.state) == len(pal_base.events) ==
                    len(pal_base.inputs) == TICKS):
                failures.append(
                    "pal-original: state/event/input stream lengths are "
                    f"{len(pal_base.state)}/{len(pal_base.events)}/"
                    f"{len(pal_base.inputs)}, expected {TICKS}/{TICKS}/{TICKS}")
            failures.extend(compare_arm(
                "pal-original", pal_base, pal_base, 25, 25, 0, 0))
            pal_60 = run(binary, pal_rom, root, "pal-60", "60",
                         args.timeout, args.verbose)
            failures.extend(compare_arm(
                "pal-60", pal_60, pal_base, 60, 25, 1, 60))
            notes.append(
                f"pal-60: {pal_60.summary.get('presents')} presents / "
                f"{pal_60.summary.get('ticks')} fixed 25 Hz ticks")
        except RuntimeError as error:
            failures.append(str(error))

    if failures:
        print("check_arbitrary_presentation_rates: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_arbitrary_presentation_rates: PASS -- exact rational "
          "presentation counts with byte-identical state/event/input/PCM")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
