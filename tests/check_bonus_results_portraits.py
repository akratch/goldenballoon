#!/usr/bin/env python3
"""Prove Wizpig and Terry own distinct cards in the real post-race flow."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
CAPTURE_FRAME = 7700
CASES = {
    "wizpig": ("MDKR_WIZPIG_TEST_PLAYER", 2),
    "terry": ("MDKR_TERRY_TEST_PLAYER", 3),
}


class PortraitError(RuntimeError):
    pass


def card_pixels(path: Path) -> list[tuple[int, int, int]]:
    width, height, pixels = read_ppm(path)
    # Single-player race-times card at the retail stopwatch portrait anchor.
    values: list[tuple[int, int, int]] = []
    for y in range(int(height * 0.72), int(height * 0.88)):
        for x in range(int(width * 0.105), int(width * 0.215)):
            offset = (y * width + x) * 3
            values.append(tuple(pixels[offset:offset + 3]))
    return values


def run_case(binary: Path, rom: Path, name: str, variable: str,
             identity: int, root: Path, timeout: int) -> None:
    case_root = root / name
    save = case_root / "save"
    frames = case_root / "frames"
    save.mkdir(parents=True)
    frames.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update({
        "LC_ALL": "C",
        "MDKR_AUDIO": "0",
        # A repo-root mdkr64.ini (e.g. FrameLimit=uncapped) must not
        # govern a frame-budgeted headless run.
        "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "MDKR_AUTOPILOT": "1",
        "MDKR_TRACE": "1",
        "MDKR_LOAD_TRACK": "5:0",
        variable: "0",
        "MDKR_SAVE_DIR": str(save),
        "MDKR_RENDERER": "gl",
        "MDKR_DUMP_FROM": str(CAPTURE_FRAME),
        "MDKR_DUMP_EVERY": "10000",
    })
    process = subprocess.run(
        [str(binary), "--headless-frames", str(CAPTURE_FRAME + 20),
         "--input-script", str(SCRIPT), "--dump-frames", str(frames),
         "--rom", str(rom)],
        cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False,
    )
    output = process.stdout or ""
    (case_root / "run.log").write_text(output, encoding="utf-8")
    if process.returncode != 0:
        raise PortraitError(f"{name}: runner exited {process.returncode}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                   "runtime error:"):
        if marker in output:
            raise PortraitError(f"{name}: emitted {marker}")
    required = (
        f"bonus_portrait: identity={identity} source=native-card "
        "size=40x40 retail=40x40",
        f"bonus_results_portrait: player=0 identity={identity} "
        "source=native-card",
    )
    for marker in required:
        if marker not in output:
            raise PortraitError(f"{name}: missing {marker!r}")

    pixels = card_pixels(frames / f"frame_{CAPTURE_FRAME}.ppm")
    if len(set(pixels)) < 200:
        raise PortraitError(f"{name}: result-card region is visually empty")
    red = sum(r > 110 and r > g * 1.25 and r > b * 1.15
              for r, g, b in pixels)
    gold = sum(r > 140 and g > 90 and b < 90 and r > g * 1.08
               for r, g, b in pixels)
    teal = sum(g > 55 and b > 45 and g > r * 1.15
               for r, g, b in pixels)
    total = len(pixels)
    if name == "wizpig":
        if red < total * 0.45 or gold < total * 0.12 or teal:
            raise PortraitError(
                f"wizpig: card lost its red/gold boss identity "
                f"(red={red}, gold={gold}, teal={teal}, total={total})")
    elif teal < total * 0.55 or gold < total * 0.03 or red > total * 0.10:
        raise PortraitError(
            f"terry: card lost its green/beak identity "
            f"(red={red}, gold={gold}, teal={teal}, total={total})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--evidence-dir")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file() or not SCRIPT.is_file():
        print("check_bonus_results_portraits: FAIL -- missing binary, ROM, or input fixture",
              file=sys.stderr)
        return 1
    temporary = args.evidence_dir is None
    root = (Path(tempfile.mkdtemp(prefix="mdkr-bonus-portraits-"))
            if temporary else Path(args.evidence_dir))
    root.mkdir(parents=True, exist_ok=True)
    try:
        for name, (variable, identity) in CASES.items():
            run_case(binary, rom, name, variable, identity, root,
                     args.timeout)
    except (OSError, subprocess.TimeoutExpired, PortraitError) as error:
        print(f"check_bonus_results_portraits: FAIL -- {error}",
              file=sys.stderr)
        print(f"evidence: {root}", file=sys.stderr)
        return 1
    print("check_bonus_results_portraits: PASS -- real post-race flow rendered "
          "distinct retail-sized Wizpig and Terry cards")
    if args.evidence_dir:
        print(f"evidence: {root}")
    if temporary:
        shutil.rmtree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
