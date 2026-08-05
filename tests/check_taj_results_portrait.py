#!/usr/bin/env python3
"""Prove Taj owns a full native results portrait in a real 2P Rankings flow."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_taj_character_select import read_ppm
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/taj_2p_split.txt"
STATE = ROOT / "tests/fixtures/taj_mod_state_unlocked.ini"
CAPTURE_FRAMES = (7100, 12750)
MIN_FRAMES = 12780


def portrait_samples(width: int, height: int, pixels: bytes,
                     bounds: tuple[float, float, float, float]) -> list[tuple[int, int, int]]:
    x0, y0, x1, y1 = bounds
    samples: list[tuple[int, int, int]] = []
    for y in range(int(height * y0), int(height * y1)):
        for x in range(int(width * x0), int(width * x1)):
            offset = (y * width + x) * 3
            samples.append(tuple(pixels[offset:offset + 3]))
    return samples


def colour_count(samples: list[tuple[int, int, int]], name: str) -> int:
    count = 0
    for red, green, blue in samples:
        if name == "purple":
            match = (blue > 70 and red > 40 and blue > green * 1.20 and
                     red > blue * 0.45)
        elif name == "skin":
            match = (red > 125 and green > 70 and green < red * 0.93 and
                     blue < green * 0.90)
        elif name == "blue":
            match = blue > 90 and blue > red * 1.20 and blue > green * 1.10
        elif name == "gold":
            # Results menus animate their global opacity, so retain the jewel
            # and warm face detail at both the bright and dim cycle phases.
            match = (red > 110 and green > 65 and blue < 90 and
                     red > green * 1.20)
        elif name == "red":
            match = red > 145 and red > green * 1.35 and red > blue * 1.35
        else:
            raise ValueError(f"unknown colour class {name}")
        count += match
    return count


# Diddy's authored result card is predominantly his red cap/shirt. Floor is a
# fraction of the same 40x40 logical card footprint the samples cover.
DIDDY_RED_MIN_COVERAGE = 0.05


def validate_capture(capture: Path) -> list[str]:
    failures: list[str] = []
    width, height, pixels = read_ppm(capture)
    # Both bounds are the same 40x40 logical result-card footprint.
    taj = portrait_samples(width, height, pixels, (0.34, 0.23, 0.46, 0.39))
    diddy = portrait_samples(width, height, pixels, (0.54, 0.23, 0.66, 0.39))
    if not taj or len(taj) != len(diddy):
        return ["portrait evidence regions are invalid"]

    taj_counts = {name: colour_count(taj, name) for name in
                  ("purple", "skin", "blue", "gold", "red")}
    diddy_counts = {name: colour_count(diddy, name) for name in
                    ("purple", "red")}
    total = len(taj)
    if taj_counts["purple"] < total * 0.12:
        failures.append("Taj card lacks the purple turban/robe")
    if taj_counts["skin"] < total * 0.08:
        failures.append("Taj card lacks a visible face/trunk")
    if taj_counts["blue"] < total * 0.25:
        failures.append("Taj card lacks its authored blue field")
    if taj_counts["gold"] < total * 0.025:
        failures.append("Taj card lacks the gold turban detail")
    if taj_counts["purple"] < diddy_counts["purple"] * 3:
        failures.append("Taj result is not visually distinct from Diddy")
    # Relative-only again: Taj's card has no red, so taj_counts["red"] is 0 and
    # `diddy_red < 0` can never be true -- the assertion passed with Diddy's
    # card entirely blank. Pin an absolute floor as well, so this fails when
    # P2's portrait stops being drawn at all.
    diddy_red_floor = int(total * DIDDY_RED_MIN_COVERAGE)
    if diddy_counts["red"] < max(taj_counts["red"] * 1.5, diddy_red_floor):
        failures.append(
            "ordinary P2 no longer renders Diddy's portrait: "
            f"diddy_red={diddy_counts['red']}, taj_red={taj_counts['red']}, "
            f"floor={diddy_red_floor}, region={total}")
    mean_difference = sum(
        abs(left[channel] - right[channel])
        for left, right in zip(taj, diddy)
        for channel in range(3)
    ) / (total * 3)
    if mean_difference < 30.0:
        failures.append(
            "Taj and Diddy portrait pixels are insufficiently distinct: "
            f"mean difference={mean_difference:.2f}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=MIN_FRAMES)
    parser.add_argument(
        "--timeout", type=int, default=300,
        help="wall-clock limit for the long two-results capture (default: 300s)",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT, STATE):
        if not path.is_file():
            print(f"check_taj_results_portrait: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1
    if args.frames < MIN_FRAMES:
        print(f"check_taj_results_portrait: FAIL -- need at least {MIN_FRAMES} frames",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-taj-results-") as temporary:
        run_dir = Path(temporary)
        save_dir = run_dir / "save"
        frames_dir = run_dir / "frames"
        save_dir.mkdir()
        frames_dir.mkdir()
        shutil.copyfile(STATE, save_dir / "taj_mod_state.ini")

        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_TRACE="1",
            MDKR_RENDERER="gl",
            MDKR_DUMP_FROM=str(CAPTURE_FRAMES[0]),
            MDKR_DUMP_EVERY=str(CAPTURE_FRAMES[1] - CAPTURE_FRAMES[0]),
            MDKR_SAVE_DIR=str(save_dir),
        )
        command = [
            str(binary), "--headless-frames", str(args.frames),
            "--input-script", str(SCRIPT), "--dump-frames", str(frames_dir),
            "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        try:
            process = subprocess.run(
                command, cwd=run_dir, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", "replace")
            print(
                "check_taj_results_portrait: FAIL -- capture exceeded "
                f"{args.timeout}s\n{output[-4000:]}",
                file=sys.stderr,
            )
            return 1
        output = process.stdout or ""
        if process.returncode != 0:
            failures.append(f"exit code {process.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in output:
                failures.append(f"fatal marker in output: {marker}")
        required = {
            "OpenGL evidence renderer": "[mdkr64] renderer backend: gl",
            "real Rankings menu": "menu_init: menuId=17",
            "retail-sized native portrait":
                "taj_portrait: source=native-taj-card size=40x40 retail=40x40",
            "Taj result ownership":
                "taj_results_portrait: player=0 identity=taj source=native-taj-card",
        }
        for label, marker in required.items():
            if marker not in output:
                failures.append(f"{label}: missing {marker!r}")
        if output.count(
                "taj_portrait: source=native-taj-card size=40x40 retail=40x40") < 2:
            failures.append(
                "Taj portrait was not reconstructed after menu/stage teardown")
        if "taj_results_portrait: player=1 identity=taj" in output:
            failures.append("ordinary P2 inherited P1's Taj portrait")

        for frame in CAPTURE_FRAMES:
            capture = frames_dir / f"frame_{frame}.ppm"
            try:
                failures.extend(
                    f"frame {frame}: {failure}"
                    for failure in validate_capture(capture))
            except (OSError, ValueError) as error:
                failures.append(
                    f"could not inspect Rankings capture at frame {frame}: {error}")

    if failures:
        for failure in failures:
            print(f"check_taj_results_portrait: FAIL -- {failure}",
                  file=sys.stderr)
        return 1
    print("check_taj_results_portrait: PASS -- two real 2P Rankings loads "
          "rendered a retail-sized native Taj card while P2 remained Diddy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
