#!/usr/bin/env python3
"""Exact arbitrary-rate presentation must remain presentation-only.

Runs the same fixed-tick route at original, common numeric caps, and the
deterministic headless stand-in for uncapped presentation.  Every arm must
produce the exact rational number of presentation opportunities while keeping
the v3 authority, ordered gameplay-event, consumed-input, and temporary PCM
streams byte-identical.  A PAL arm proves that 60 Hz no longer has to be
rounded onto, or refused by, the 50 Hz source-field grid.

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
        policy: str | None, timeout: int, verbose: bool) -> Result:
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
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH=HASH_VERSION,
    )
    if policy is not None:
        env["MDKR_PRESENT_RATE"] = policy
    command = [
        str(binary), "--headless-ticks", str(TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        setting = policy if policy is not None else "original"
        print(f"$ ({label}) MDKR_PRESENT_RATE={setting} {' '.join(command)}",
              flush=True)
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
                policy_rate: int) -> list[str]:
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
    for key in ("pending", "multidue", "lead", "lag", "catchup", "skips",
                "rebases", "blocked", "updatebad"):
        if summary.get(key, 0) != 0:
            failures.append(f"{label}: {key}={summary.get(key)}, expected 0")
    for key, value in (("presents", expected), ("entries", expected),
                       ("fieldhz", 50 if tick_rate == 25 else 60),
                       ("tickfields", 2), ("presentkind", policy_kind),
                       ("presentrate", policy_rate)):
        if summary.get(key) != value:
            failures.append(
                f"{label}: {key}={summary.get(key)}, expected {value}")
    if rate > tick_rate and summary.get("interp", 0) == 0:
        failures.append(f"{label}: subloop issued no interpolated/replayed presents")

    audio = result.audio
    for key, value in (("fields", TICKS * 2), ("due", TICKS),
                       ("serviced", TICKS - 1), ("pending", 1),
                       ("retired", 0), ("rebases", 0),
                       ("calls", TICKS - 1), ("idle", 0),
                       ("notready", 0), ("dropped", 0),
                       ("quantumfields", 2)):
        if audio.get(key) != value:
            failures.append(
                f"{label}: audio {key}={audio.get(key)}, expected {value}")
    if audio.get("samples") != baseline.audio.get("samples"):
        failures.append(
            f"{label}: audio samples={audio.get('samples')}, expected "
            f"{baseline.audio.get('samples')}")

    if rate > tick_rate:
        replay = result.replay
        packet = result.packet
        if replay.get("walks", 0) == 0:
            failures.append(f"{label}: replay walk never ran")
        for key in ("freezefail", "restorefail"):
            if replay.get(key, 0) != 0:
                failures.append(f"{label}: replay {key}={replay.get(key)}")
        for key in ("freezefail", "deformcollision", "effectcollision"):
            if packet.get(key, 0) != 0:
                failures.append(f"{label}: packet {key}={packet.get(key)}")
    return failures


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
