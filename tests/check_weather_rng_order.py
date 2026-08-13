#!/usr/bin/env python3
"""Weather-enabled authored RNG-order and presentation-invariance oracle.

Wizpig 1 (level 37) is the only normal level route that exercises rain.  The
test records the raw authoritative state hash plus the seed immediately around
every splash placement roll and lightning timer reset.  Only the digest and
row counts are stored here; no ROM-derived state or event stream is shipped.

The intentionally wrong ``MDKR_TEST_WEATHER_RNG_EARLY`` arm is a positive
control for the exact audited regression: weather before objects instead of
objects -> weather -> HUD.  It must diverge from the frozen Original oracle.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TICKS = 4200
EXPECTED_STATE_ROWS = TICKS
EXPECTED_WEATHER_ROWS = 813
# SHA-256 over every [SIMHASH] row followed by every [WEATHER-RNG] row, each
# newline terminated. This is the reviewed object -> weather -> HUD route after
# restoring retail racer_sound_car() and its ares-proven shared-RNG draws. The
# earlier oracle came from the port build that incorrectly skipped car audio.
# Re-frozen when line-particle orientation was restored to the N64's MSB-first
# bit positions (issue #24): the prior digest was captured with every line
# particle on the wrong local axis. Reverting that one accessor reproduces the
# old digest exactly, so the orientation restore is the whole delta.
# Re-frozen 2026-08-13 for the release fixes ba28eae shipped without a
# re-freeze (first-parent bisect: b66b1b1 reproduces the prior digest exactly,
# ba28eae diverges; both endpoints from scratch builds). That integration
# carries the door/tree unstick and the #30 audio-budget fixes -- intended,
# release-noted simulation changes. The divergence begins at tick 7 of level
# load, moves no v1/v2 field (positions, rotations, integrators, and the RNG
# seed stream are bit-identical; both control arms still diverge as designed)
# and lives entirely in v3's added authority fields, so RNG order -- the thing
# this gate polices -- is untouched. Skip-render and 30/60 presentation
# invariance were re-verified on the new stream before this digest was pinned.
EXPECTED_ORIGINAL_SHA256 = (
    "253847b6b10ea7edcf695816159f636b74a799cbde76245774b955e91a525c3c"
)
# Positive control for the scheduler/fixture boundary.  Delaying every positive
# input edge by one ticket used to be easy to do accidentally when the host
# began publishing input after accounting the completed simulation pass.  It
# changes the race-entry window and removes one weather row.  Freeze that exact
# broken direction so restoring the accepted oracle cannot be faked by updating
# the expected digest to whichever route happened to run.
EXPECTED_LATE_PHASE_WEATHER_ROWS = 812
# Re-frozen alongside EXPECTED_ORIGINAL_SHA256 above: the same authored delta
# moves this frozen broken direction too, and its row count still drops by one.
EXPECTED_LATE_PHASE_SHA256 = (
    "a9f8d537aa654275c1fe2d9c950b370c879752a04527e5762a9a1f7075c46a67"
)


@dataclass(frozen=True)
class Result:
    state: tuple[str, ...]
    weather: tuple[str, ...]

    def digest(self) -> str:
        payload = "\n".join((*self.state, *self.weather)) + "\n"
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def run_arm(binary: Path, rom: Path, root: Path, label: str, script: Path,
            cadence: str, rate: str | None, extra_env: dict[str, str],
            timeout: int, verbose: bool,
            artifacts_dir: Path | None = None) -> Result:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="37",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_SIMULATION_CADENCE=cadence,
        MDKR_STATE_HASH="3",
        MDKR_SYNTH_FIELDS="1" if cadence == "enhanced" else "2",
        MDKR_WEATHER_RNG_TRACE="1",
    )
    if rate is not None:
        env["MDKR_PRESENT_RATE"] = rate
    env.update(extra_env)
    command = [
        str(binary), "--headless-ticks", str(TICKS),
        "--input-script", str(script), "--rom", str(rom),
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
    if artifacts_dir is not None:
        artifacts_dir.mkdir(parents=True, exist_ok=True)
        (artifacts_dir / f"{label}.log").write_text(
            output, encoding="utf-8")
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                   "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    state = tuple(line for line in output.splitlines()
                  if line.startswith("[SIMHASH]"))
    weather = tuple(line for line in output.splitlines()
                    if line.startswith("[WEATHER-RNG]"))
    if artifacts_dir is not None:
        (artifacts_dir / f"{label}.state.txt").write_text(
            "\n".join(state) + "\n", encoding="utf-8")
        (artifacts_dir / f"{label}.weather.txt").write_text(
            "\n".join(weather) + "\n", encoding="utf-8")
    if len(state) != EXPECTED_STATE_ROWS:
        raise RuntimeError(
            f"{label}: expected {EXPECTED_STATE_ROWS} state rows, "
            f"got {len(state)}")
    return Result(state, weather)


def write_late_phase_control(source: Path, target: Path) -> None:
    """Write the historical one-ticket-late version of an authored fixture."""

    output: list[str] = []
    for line in source.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and fields[0].isdigit():
            fields[0] = str(int(fields[0]) + 1)
            line = " ".join(fields)
        output.append(line)
    target.write_text("\n".join(output) + "\n", encoding="utf-8")


def first_difference(left: Result, right: Result) -> str:
    for stream_name in ("state", "weather"):
        a = getattr(left, stream_name)
        b = getattr(right, stream_name)
        for index, pair in enumerate(zip(a, b)):
            if pair[0] != pair[1]:
                return f"{stream_name} row {index}"
        if len(a) != len(b):
            return f"{stream_name} length {len(a)} vs {len(b)}"
    return "no difference"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--artifacts-dir", type=Path,
        help="retain complete logs plus parsed state/weather streams")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not path.exists():
            print(f"check_weather_rng_order: FAIL\n  - missing {path}")
            return 1

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-weather-rng-") as tmp:
            root = Path(tmp)
            late_script = root / "one-ticket-late.txt"
            write_late_phase_control(SCRIPT, late_script)
            original = run_arm(binary, rom, root, "original", SCRIPT,
                               "original", None, {}, args.timeout,
                               args.verbose, args.artifacts_dir)
            skipped = run_arm(
                binary, rom, root, "skip-render", SCRIPT, "original", None,
                {"MDKR_TEST_SKIP_RENDER": "odd"}, args.timeout, args.verbose,
                args.artifacts_dir)
            rate30 = run_arm(binary, rom, root, "original-30", SCRIPT,
                             "original", "30", {}, args.timeout,
                             args.verbose, args.artifacts_dir)
            rate60 = run_arm(binary, rom, root, "original-60", SCRIPT,
                             "original", "60", {}, args.timeout,
                             args.verbose, args.artifacts_dir)
            wrong_order = run_arm(
                binary, rom, root, "weather-early-control", SCRIPT,
                "original", None,
                {"MDKR_TEST_WEATHER_RNG_EARLY": "1"},
                args.timeout, args.verbose, args.artifacts_dir)
            phase_late = run_arm(
                binary, rom, root, "one-ticket-late-control", late_script,
                "original", None, {}, args.timeout, args.verbose,
                args.artifacts_dir)
            enhanced30 = run_arm(binary, rom, root, "enhanced-30", SCRIPT,
                                 "enhanced", "30", {}, args.timeout,
                                 args.verbose, args.artifacts_dir)
            enhanced60 = run_arm(binary, rom, root, "enhanced-60", SCRIPT,
                                 "enhanced", "60", {}, args.timeout,
                                 args.verbose, args.artifacts_dir)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"check_weather_rng_order: FAIL\n  - {error}")
        return 1

    failures: list[str] = []
    splash_rows = [row for row in original.weather if "kind=splash" in row]
    lightning_rows = [row for row in original.weather
                      if "kind=lightning" in row]
    if not splash_rows:
        failures.append("Original route reached no splash RNG roll")
    if not lightning_rows:
        failures.append("Original route reached no lightning timer reset")
    if len(original.weather) != EXPECTED_WEATHER_ROWS:
        failures.append(
            f"Original weather row count {len(original.weather)} != frozen "
            f"count {EXPECTED_WEATHER_ROWS}")

    for label, result in (("skip-render", skipped), ("30 FPS", rate30),
                          ("60 FPS", rate60)):
        if result != original:
            failures.append(
                f"{label} changed Original authoritative weather/state at "
                f"{first_difference(original, result)}")

    if enhanced30 != enhanced60:
        failures.append(
            "enhanced 30/60 presentation rates diverged at "
            f"{first_difference(enhanced30, enhanced60)}")
    if not any("kind=splash" in row for row in enhanced30.weather):
        failures.append("enhanced route reached no splash RNG roll")
    if not any("kind=lightning" in row for row in enhanced30.weather):
        failures.append("enhanced route reached no lightning timer reset")
    if wrong_order == original:
        failures.append(
            "wrong-order positive control matched Original; oracle cannot "
            "detect weather-before-object RNG drift")
    phase_late_digest = phase_late.digest()
    if (phase_late == original or
            len(phase_late.weather) != EXPECTED_LATE_PHASE_WEATHER_ROWS or
            phase_late_digest != EXPECTED_LATE_PHASE_SHA256):
        failures.append(
            "one-ticket-late input control did not reproduce the frozen "
            "broken direction: "
            f"rows={len(phase_late.weather)} digest={phase_late_digest}")

    original_digest = original.digest()
    if original_digest != EXPECTED_ORIGINAL_SHA256:
        failures.append(
            "Original weather oracle digest changed: "
            f"{original_digest} != {EXPECTED_ORIGINAL_SHA256}")
    # A one-byte mutation must be rejected by the same frozen digest check.
    mutated = Result((original.state[0] + "!", *original.state[1:]),
                     original.weather)
    if mutated.digest() == EXPECTED_ORIGINAL_SHA256:
        failures.append("one-byte oracle mutation was not detected")

    if failures:
        print("check_weather_rng_order: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        "check_weather_rng_order: PASS — Original object -> weather -> HUD "
        f"oracle {original_digest} ({len(splash_rows)} splash rolls, "
        f"{len(lightning_rows)} lightning resets); byte-identical under "
        "skip-render and 30/60 presentation; enhanced cadence is 30/60 "
        "presentation-invariant; weather-order, one-ticket-late, and one-byte "
        "controls diverge")
    return 0


if __name__ == "__main__":
    sys.exit(main())
