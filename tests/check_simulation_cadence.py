#!/usr/bin/env python3
"""End-to-end containment gate for source-region simulation cadence."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


PACE_INIT_RE = re.compile(
    r"pace init: mode=(\w+) cadence=(\w+) minFields=(\d+) "
    r"compensate=(\d+) synthFields=(\d+) fieldHz=(\d+) sourceFieldHz=(\d+)"
)
PACE_RE = re.compile(r"\[PACE\] frame=\d+ R=(\d+) wf=(\d+)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|runtime error:")
AUDIO_RE = re.compile(r"\[AUDIO-SERVICE\] (.*)")


def clean_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR_")}
    env.update(
        MDKR_AUDIO="0", MDKR_AUDIO_SERVICE_TRACE="1", MDKR_TRACE="1")
    if extra:
        env.update(extra)
    return env


def run_probe(
    binary: Path,
    rom: Path,
    extra: dict[str, str] | None = None,
) -> tuple[tuple[str, str, int, int, int, int, int], list[tuple[int, int]], str]:
    with tempfile.TemporaryDirectory(prefix="mdkr-cadence-") as raw:
        proc = subprocess.run(
            [
                str(binary),
                "--headless-frames", "3",
                "--rom", str(rom),
            ],
            cwd=raw,
            env=clean_env(extra),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=60,
            check=False,
        )
    output = proc.stdout
    if proc.returncode != 0:
        raise AssertionError(f"probe exited {proc.returncode}\n{output[-4000:]}")
    bad = BAD_RE.search(output)
    if bad:
        raise AssertionError(f"probe reported {bad.group(0)}\n{output[-4000:]}")
    match = PACE_INIT_RE.search(output)
    if match is None:
        raise AssertionError(f"probe omitted pacing initialization\n{output[-4000:]}")
    init = (
        match.group(1),
        match.group(2),
        *(int(match.group(index)) for index in range(3, 8)),
    )
    rows = [(int(rate), int(wall)) for rate, wall in PACE_RE.findall(output)]
    if len(rows) != 3:
        raise AssertionError(f"expected three pacing rows, got {rows}")
    return init, rows, output


def find_pal_rom(directory: Path) -> Path:
    candidates = sorted(directory.glob("*.z64"))
    for candidate in candidates:
        name = candidate.name.lower()
        if ("europe" in name or "pal" in name) and (
            "rev 1" in name or "v80" in name
        ):
            return candidate.resolve()
    raise AssertionError(
        f"no supported PAL v80 ROM found below {directory}; "
        "the release cadence gate requires both source clocks"
    )


def audio_summary(output: str) -> dict[str, int]:
    matches = list(AUDIO_RE.finditer(output))
    if not matches:
        raise AssertionError("probe omitted [AUDIO-SERVICE] summary")
    values: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        if separator:
            values[key] = int(value)
    return values


def require_audio_clock(
    output: str, *, fields: int, due: int, serviced: int,
    pending: int, calls: int, idle: int,
) -> None:
    summary = audio_summary(output)
    expected = {
        "fields": fields,
        "due": due,
        "serviced": serviced,
        "pending": pending,
        "calls": calls,
        "idle": idle,
        "notready": 0,
        "retired": 0,
        "dropped": 0,
        "quantumfields": 2,
    }
    for key, value in expected.items():
        if summary.get(key) != value:
            raise AssertionError(
                f"audio service {key}={summary.get(key)}, expected {value}: "
                f"{summary}")


def check_video_preset_independence(binary: Path) -> None:
    for mode in ("--pure", "--restored", "--remastered"):
        proc = subprocess.run(
            [str(binary), "--video-list", mode],
            env=clean_env({"MDKR_SIMULATION_CADENCE": "enhanced"}),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
        cadence_line = next(
            (
                line for line in proc.stdout.splitlines()
                if "Gameplay.SimulationCadence" in line
            ),
            "",
        )
        if proc.returncode != 0 or "enhanced" not in cadence_line or "[env]" not in cadence_line:
            raise AssertionError(
                f"{mode} rewrote gameplay cadence: {cadence_line!r}\n{proc.stdout}"
            )


def check_oracle_manifest(root: Path) -> None:
    routes = sorted((root / "tools" / "oracle_routes").glob("*.json"))
    if not routes:
        raise AssertionError("oracle route manifest is empty")
    for route in routes:
        proc = subprocess.run(
            [
                sys.executable,
                str(root / "tools" / "dkr_oracle_route.py"),
                "validate",
                str(route),
            ],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(
                f"oracle route lacks explicit cadence: {route}\n{proc.stdout}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--roms", default="build/roms")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    binary = Path(resolve_binary(args.build)).resolve()
    ntsc_rom = Path(args.rom).resolve()
    roms = Path(args.roms).resolve()
    failures: list[str] = []

    try:
        pal_rom = find_pal_rom(roms)

        init, rows, output = run_probe(binary, ntsc_rom)
        if init != ("synth", "original", 2, 1, 2, 60, 60):
            raise AssertionError(f"NTSC original initialization mismatch: {init}")
        if rows != [(2, 2)] * 3:
            raise AssertionError(f"NTSC original did not inject two fields: {rows}")
        if "source video: NTSC (60 Hz fields)" not in output:
            raise AssertionError("NTSC ROM did not publish its source clock")
        require_audio_clock(
            output, fields=6, due=3, serviced=2, pending=1,
            calls=2, idle=0)

        init, rows, output = run_probe(binary, pal_rom)
        if init != ("synth", "original", 2, 1, 2, 50, 50):
            raise AssertionError(f"PAL original initialization mismatch: {init}")
        if rows != [(2, 2)] * 3:
            raise AssertionError(f"PAL original did not inject two fields: {rows}")
        if "source video: PAL (50 Hz fields)" not in output:
            raise AssertionError("PAL ROM did not publish its source clock")
        require_audio_clock(
            output, fields=6, due=3, serviced=2, pending=1,
            calls=2, idle=0)

        init, rows, output = run_probe(
            binary,
            ntsc_rom,
            {
                "MDKR_SIMULATION_CADENCE": "enhanced",
                "MDKR_SYNTH_FIELDS": "1",
            },
        )
        if init != ("synth", "enhanced", 1, 1, 1, 60, 60) or rows != [(1, 1)] * 3:
            raise AssertionError(f"explicit enhanced arm mismatch: {init}, {rows}")
        require_audio_clock(
            output, fields=3, due=1, serviced=1, pending=0,
            calls=2, idle=1)

        init, rows, _ = run_probe(
            binary,
            ntsc_rom,
            {"MDKR_SYNTH_FIELDS": "1"},
        )
        if init[4] != 2 or rows != [(2, 2)] * 3:
            raise AssertionError(
                f"synthetic override undercut original minimum: {init}, {rows}"
            )

        init, rows, _ = run_probe(
            binary,
            ntsc_rom,
            {"MDKR_FIELD_HZ": "120"},
        )
        if init[5:7] != (120, 60):
            raise AssertionError(f"valid diagnostic field clock missing: {init}")

        init, _, _ = run_probe(
            binary,
            pal_rom,
            {"MDKR_FIELD_HZ": "999"},
        )
        if init[5:7] != (50, 50):
            raise AssertionError(f"invalid field clock did not fail safe: {init}")

        init, rows, _ = run_probe(
            binary,
            ntsc_rom,
            {"MDKR_VI_PACE": "off"},
        )
        if init[3] != 0 or rows != [(1, 2)] * 3:
            raise AssertionError(
                "MDKR_VI_PACE=off must disable compensation while retaining "
                f"the original wall-time cadence: {init}, {rows}"
            )

        check_video_preset_independence(binary)
        check_oracle_manifest(root)
    except (AssertionError, OSError, subprocess.SubprocessError) as exc:
        failures.append(str(exc))

    if failures:
        print("check_simulation_cadence: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "check_simulation_cadence: PASS — NTSC 60/PAL 50 source clocks, "
        "original/enhanced synthetic policy, independent two-field audio "
        "service, preset isolation, diagnostic semantics, and explicit oracle "
        "cadence"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
