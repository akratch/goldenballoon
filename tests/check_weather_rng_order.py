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

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TICKS = 4200
EXPECTED_STATE_ROWS = TICKS
EXPECTED_WEATHER_ROWS = 812
# SHA-256 over every [SIMHASH] row followed by every [WEATHER-RNG] row, each
# newline terminated. Filled from the reviewed object -> weather -> HUD route.
EXPECTED_ORIGINAL_SHA256 = (
    "8994e5b32138dfdd09b24f64bd43ce7a4e8dca2c7c297128d2c3d8953963fc64"
)


@dataclass(frozen=True)
class Result:
    state: tuple[str, ...]
    weather: tuple[str, ...]

    def digest(self) -> str:
        payload = "\n".join((*self.state, *self.weather)) + "\n"
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def run_arm(binary: Path, rom: Path, root: Path, label: str,
            cadence: str, rate: str | None, extra_env: dict[str, str],
            timeout: int, verbose: bool) -> Result:
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
        "--input-script", str(SCRIPT), "--rom", str(rom),
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
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                   "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    state = tuple(line for line in output.splitlines()
                  if line.startswith("[SIMHASH]"))
    weather = tuple(line for line in output.splitlines()
                    if line.startswith("[WEATHER-RNG]"))
    if len(state) != EXPECTED_STATE_ROWS:
        raise RuntimeError(
            f"{label}: expected {EXPECTED_STATE_ROWS} state rows, "
            f"got {len(state)}")
    return Result(state, weather)


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
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
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
            original = run_arm(binary, rom, root, "original", "original",
                               None, {}, args.timeout, args.verbose)
            skipped = run_arm(
                binary, rom, root, "skip-render", "original", None,
                {"MDKR_TEST_SKIP_RENDER": "odd"}, args.timeout, args.verbose)
            rate30 = run_arm(binary, rom, root, "original-30", "original",
                             "30", {}, args.timeout, args.verbose)
            rate60 = run_arm(binary, rom, root, "original-60", "original",
                             "60", {}, args.timeout, args.verbose)
            wrong_order = run_arm(
                binary, rom, root, "weather-early-control", "original", None,
                {"MDKR_TEST_WEATHER_RNG_EARLY": "1"},
                args.timeout, args.verbose)
            enhanced30 = run_arm(binary, rom, root, "enhanced-30", "enhanced",
                                 "30", {}, args.timeout, args.verbose)
            enhanced60 = run_arm(binary, rom, root, "enhanced-60", "enhanced",
                                 "60", {}, args.timeout, args.verbose)
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
        "presentation-invariant; wrong-order and one-byte controls diverge")
    return 0


if __name__ == "__main__":
    sys.exit(main())
