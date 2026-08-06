#!/usr/bin/env python3
"""Measure Taj's sustained speed against same-route retail controls.

This is deliberately a paired real-ROM test, not a unit assertion about the
constant. Each car, hovercraft, and plane arm drives Ancient Lake with the same
autopilot, cadence, input fixture, and frame window. The only independent
variable is Taj identity. The p95 of non-boosting racer velocity must land
within two percent of the advertised 1.35x sustained-speed profile.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
VEHICLES = {0: "car", 1: "hovercraft", 2: "plane"}
FRAMES = 5200
WINDOW_START = 3500
RATIO_MIN = 1.32
RATIO_MAX = 1.38

BOOST_RE = re.compile(
    r"\[BOOST\] frame=(\d+) timer=(-?\d+) type=(-?\d+) vel=(\S+) "
    r"x=(\S+) y=(\S+) z=(\S+) surf=(-?\d+) grounded=(-?\d+) "
    r"start=(-?\d+)"
)


def percentile_95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * 0.95)]


def run_arm(binary: str, rom: str, vehicle: int, taj: bool,
            frames: int, window_start: int, verbose: bool) -> tuple[float, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_PRESENT_RATE="original",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_BOOST_TRACE="1",
        MDKR_LOAD_TRACK=f"5:{vehicle}",
    )
    if taj:
        env["MDKR_TAJ_TEST_PLAYER"] = "0"
        env["MDKR_TAJ_PHYSICS_TRACE"] = "1"
    command = [
        binary, "--headless-frames", str(frames), "--input-script",
        str(SCRIPT), "--rom", rom,
    ]
    if verbose:
        print("$ " + " ".join(command) +
              ("  # Taj" if taj else "  # stock"), flush=True)
    process = subprocess.run(
        command, cwd=ROOT, env=env, text=True, capture_output=True,
        timeout=90, check=False,
    )
    output = (process.stdout or "") + (process.stderr or "")
    if process.returncode != 0:
        raise RuntimeError(f"{VEHICLES[vehicle]} {'Taj' if taj else 'stock'} "
                           f"exited {process.returncode}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                   "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{VEHICLES[vehicle]} arm emitted {marker}")
    expected_dispatch = f"[PVEH] frame="
    if expected_dispatch not in output or f"vehicleID={vehicle}" not in output:
        raise RuntimeError(f"{VEHICLES[vehicle]} arm missed its vehicle dispatch")
    if taj:
        marker = f"[TAJPHYS] active racer=0 player=0 vehicle={vehicle}"
        if marker not in output:
            raise RuntimeError(f"{VEHICLES[vehicle]} Taj identity was not active")
    elif "[TAJPHYS] active" in output:
        raise RuntimeError(f"{VEHICLES[vehicle]} stock control activated Taj")

    velocities: list[float] = []
    # NOTE: this fixture takes no zip pad. Measured on the shipped route, all
    # 2578 [BOOST] rows of a full car run report timer=0, so the boosted-peak
    # behaviour Taj's clamp used to eat CANNOT be witnessed here; it is covered
    # at the unit level by tests/test_taj_physics.c instead
    # (taj_physics_speed_cap).
    for match in BOOST_RE.finditer(output):
        frame = int(match.group(1))
        boost_timer = int(match.group(2))
        velocity = abs(float(match.group(4)))
        race_start = int(match.group(10))
        if frame >= window_start and boost_timer == 0 and race_start == 0:
            velocities.append(velocity)
    if len(velocities) < 500:
        raise RuntimeError(
            f"{VEHICLES[vehicle]} arm produced only {len(velocities)} "
            "non-boosting measurement rows"
        )
    return percentile_95(velocities), output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--window-start", type=int, default=WINDOW_START)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = resolve_binary(args.build)
    rom = str(Path(args.rom).resolve())
    for path in (Path(binary), Path(rom), SCRIPT):
        if not path.exists():
            print(f"check_taj_speed_profile: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1
    if args.window_start >= args.frames - 500:
        print("check_taj_speed_profile: FAIL -- measurement window is too short",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    rows: list[tuple[str, float, float, float]] = []
    for vehicle, name in VEHICLES.items():
        try:
            stock, _ = run_arm(binary, rom, vehicle, False, args.frames,
                               args.window_start, args.verbose)
            taj, _ = run_arm(binary, rom, vehicle, True, args.frames,
                             args.window_start, args.verbose)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(str(error))
            continue
        ratio = taj / stock if stock > 0.0 else 0.0
        rows.append((name, stock, taj, ratio))
        if not (11.5 <= stock <= 15.0):
            failures.append(
                f"{name}: stock p95 {stock:.4f} escaped its retail control band"
            )
        if not (RATIO_MIN <= ratio <= RATIO_MAX):
            failures.append(
                f"{name}: Taj/stock p95 ratio {ratio:.4f} is outside "
                f"[{RATIO_MIN:.2f}, {RATIO_MAX:.2f}]"
            )

    for name, stock, taj, ratio in rows:
        print(f"  {name:<11} stock={stock:7.4f} Taj={taj:7.4f} "
              f"ratio={ratio:.4f}x")
    if failures:
        for failure in failures:
            print(f"check_taj_speed_profile: FAIL -- {failure}",
                  file=sys.stderr)
        return 1
    print("check_taj_speed_profile: PASS -- all three real vehicle paths "
          "sustain Taj within 2% of 1.35x their paired retail controls")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
