#!/usr/bin/env python3
"""Prove the real two-player Adventure HUD renders Taj's native portrait."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_taj_character_select import read_ppm
from check_taj_results_portrait import colour_count, portrait_samples
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/taj_p2_adventure_probe.txt"
STATE = ROOT / "tests/fixtures/taj_mod_state_unlocked.ini"
CAPTURE_FRAME = 7400
FRAMES = 7480
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12;12:E5"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT, STATE):
        if not path.is_file():
            print(f"check_taj_hud_portrait: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1
    if args.frames < FRAMES:
        print(f"check_taj_hud_portrait: FAIL -- need at least {FRAMES} frames",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-taj-hud-") as temporary:
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
            MDKR_TRACE="1",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_TAJ_P2_LEAD_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
            MDKR_DUMP_FROM=str(CAPTURE_FRAME),
            MDKR_DUMP_EVERY="100",
        )
        command = [
            str(binary), "--headless-frames", str(args.frames),
            "--input-script", str(SCRIPT), "--dump-frames", str(frames_dir),
            "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=120, check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            failures.append(f"exit code {process.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in output:
                failures.append(f"fatal marker in output: {marker}")
        required = {
            "real P2 Taj selection":
                "taj_select: player=1 controller=1 selected=1 donor=9",
            "real Adventure hub":
                "level_load: levelId=0 numPlayers=0",
            "native HUD identity":
                "taj_hud_portrait: player=1 identity=taj source=native-taj-card",
        }
        for label, marker in required.items():
            if marker not in output:
                failures.append(f"{label}: missing {marker!r}")
        if "taj_adv_handoff:" in output:
            failures.append("observation-only HUD arm unexpectedly changed Adventure lead")

        try:
            width, height, pixels = read_ppm(
                frames_dir / f"frame_{CAPTURE_FRAME}.ppm")
            samples = portrait_samples(
                width, height, pixels, (0.755, 0.0, 0.88, 0.15))
            total = len(samples)
            counts = {name: colour_count(samples, name) for name in
                      ("purple", "skin", "blue", "gold")}
            minimums = {
                "purple": 0.15,
                "skin": 0.08,
                "blue": 0.25,
                "gold": 0.025,
            }
            for name, minimum in minimums.items():
                if counts[name] < total * minimum:
                    failures.append(
                        f"HUD Taj card lacks {name} evidence: "
                        f"{counts[name]}/{total}")
        except (OSError, ValueError) as error:
            failures.append(f"could not inspect HUD capture: {error}")

    if failures:
        for failure in failures:
            print(f"check_taj_hud_portrait: FAIL -- {failure}",
                  file=sys.stderr)
        return 1
    print("check_taj_hud_portrait: PASS -- real P2 Adventure HUD rendered "
          "Taj's native portrait without changing the lead state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
