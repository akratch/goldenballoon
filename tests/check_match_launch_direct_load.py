#!/usr/bin/env python3
"""Prove the V3 launcher descriptor, not retail menus, selects an online race."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_4p_split.txt"


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr64-match-launch-") as temp:
            run_dir = Path(temp)
            preferences = run_dir / "preferences"
            saves = run_dir / "saves"
            preferences.mkdir()
            saves.mkdir()
            environment = clean_environment(
                LC_ALL="C",
                MDKR64_HIDDEN="1",
                MDKR_APP_AUTOPLAY="1",
                MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
                MDKR_APP_AUTOPLAY_TICKS="2800",
                MDKR_APP_PREFS_DIR=str(preferences),
                MDKR_APP_TEST_ONLINE_LAUNCH_V3="1",
                MDKR_APP_TEST_ONLINE_LOCAL_MASK="0x1",
                MDKR_APP_TEST_ONLINE_LOOPBACK_INPUTS="1",
                MDKR_APP_TEST_ONLINE_MANIFEST_TRACK="6",
                MDKR_APP_TEST_ONLINE_MANIFEST_VEHICLE_MASK="0x3",
                MDKR_APP_TEST_ONLINE_VIEWPORT_MASK="0x1",
                MDKR_AUDIO="0",
                MDKR_AUTOPILOT="1",
                MDKR_PRESENT_RATE="original",
                MDKR_RENDERER="gl",
                MDKR_ROM=str(rom),
                MDKR_SAVE_DIR=str(saves),
                MDKR_TEST_SCRIPT_ONLY_INPUT="1",
                MDKR_TRACE_VEHICLE="1",
                MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            )
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL match launch direct load: {error}", file=sys.stderr)
        return 1

    output = process.stdout or ""
    required = (
        "[NET-LAUNCH] epoch=1 ",
        "track=6 selections=0:0/0,1:1/0,2:2/0,3:3/0",
        "[NET-SELECTIONS] epoch=1 racers="
        "0:0/0,1:1/0,2:2/0,3:3/0 source=launch-descriptor",
        "[ROLLBACK] online race: loadedTrack=6 raceType=0 authoredHz=30",
        "[PVEH] frame=", "player=0 vehicleID=0",
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
    )
    forbidden = (
        "MDKR_LOAD_TRACK", "online race admission rejected", "[FATAL]",
        "[CRASH]", "AddressSanitizer", "runtime error:",
    )
    if process.returncode != 0 or any(marker not in output for marker in required):
        print("FAIL match launch direct load: missing production witness",
              file=sys.stderr)
        print(output[-16000:], file=sys.stderr)
        return 1
    if any(marker in output for marker in forbidden):
        print("FAIL match launch direct load: forbidden diagnostic/test seam",
              file=sys.stderr)
        print(output[-16000:], file=sys.stderr)
        return 1
    print("match launch V3 direct-load gate passed: track=6 characters=0,1,2,3 vehicles=car")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
