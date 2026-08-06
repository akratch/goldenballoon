#!/usr/bin/env python3
"""Prove P2-led Adventure rebinds a real selected Taj without virtual IDs.

The normal campaign handoff happens after P2 wins a race. This focused native
fixture enters JOINTVENTURE through the retail Magic Codes UI, selects Taj with
P2 from the visible roster, reaches the hub, then uses the test-only
``MDKR_TAJ_P2_LEAD_HANDOFF`` seam to invoke *retail* ``swap_lead_player()`` at
the safe hub boundary. The following race is a normal game load: DKR's own
``obj_init_racer()`` must map the newly first settings row to live controller
port 2. The trace makes that mapping and the no-virtual-ID boundary explicit.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "taj_p2_adventure_probe.txt"
STATE = ROOT / "tests" / "fixtures" / "taj_mod_state_unlocked.ini"
FRAMES = 9000
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12;12:E5"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)
IDENTITY = (
    "taj_adv_identity: lead=1 settings_taj0=1 settings_taj1=0 "
    "live_taj0=0 live_taj1=1 racer_slot=0 racer_port=1 "
    "ids=9,9,9 retail_ids=1"
)


def clean_env() -> dict[str, str]:
    return {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR_", "GE007_"))
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    failures: list[str] = []
    for path in (binary, rom, SCRIPT, STATE):
        if not path.is_file():
            failures.append(f"missing {path}")
    if args.frames < FRAMES:
        failures.append(f"need at least {FRAMES} frames")
    if failures:
        for failure in failures:
            print(f"check_taj_p2_adventure: FAIL -- {failure}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="mdkr-taj-p2-adventure-") as temp:
        run_dir = Path(temp)
        save_dir = run_dir / "save"
        save_dir.mkdir()
        shutil.copyfile(STATE, save_dir / "taj_mod_state.ini")
        env = clean_env()
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_TRACE="1",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_TAJ_P2_LEAD_HANDOFF="1",
            MDKR_TAJ_P2_LEAD_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
        )
        command = [
            str(binary), "--headless-frames", str(args.frames),
            "--input-script", str(SCRIPT), "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        try:
            process = subprocess.run(
                command, cwd=run_dir, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=max(120, args.frames // 20), check=False,
            )
            output = process.stdout or ""
        except subprocess.TimeoutExpired as error:
            process = None
            output = (error.stdout or "")
            if isinstance(output, bytes):
                output = output.decode("utf-8", "replace")
            failures.append("timed out")

    if process is not None and process.returncode != 0:
        failures.append(f"exit code {process.returncode}")
    bad = BAD_RE.search(output)
    if bad is not None:
        failures.append(f"fatal runtime marker {bad.group(0)!r}")
    if "magic_code_submit: accepted=1 id=24" not in output:
        failures.append("JOINTVENTURE did not pass through the retail Magic Codes UI")
    selected = "taj_select: player=1 controller=1 selected=1 donor=9"
    if selected not in output:
        failures.append("P2 did not select visible Taj with donor character 9")
    if "taj_select: player=0 controller=0 selected=1" in output:
        failures.append("P1 unexpectedly selected Taj")
    handoff = (
        "taj_adv_handoff: source=retail_swap lead=1 "
        "settings_taj0=1 settings_taj1=0"
    )
    if handoff not in output:
        failures.append("retail lead handoff did not swap the stable Taj row")
    if IDENTITY not in output:
        failures.append("P2-led race did not bind Taj from settings row 0 to live port 1")
    if "level_load: levelId=5 numPlayers=0" not in output:
        failures.append("the first Adventure race never loaded")
    if "hud_init: hudPlayers=1 numViewports=2" not in output:
        failures.append("the P2-led race did not retain two-player split-screen")

    order = [output.find(marker) for marker in (selected, handoff, IDENTITY)]
    if -1 not in order and order != sorted(order):
        failures.append("selection, lead handoff, and rebound identity are out of order")
    if "taj_test_player:" in output:
        failures.append("test-player bootstrap contaminated the real selection path")

    if failures:
        for failure in failures:
            print(f"check_taj_p2_adventure: FAIL -- {failure}", file=sys.stderr)
        return 1
    print("check_taj_p2_adventure: PASS -- P2 selected visible Taj, retail lead "
          "handoff swapped the stable row, and the next real Adventure race "
          "bound slot 0 to live port 1 with only retail character IDs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
