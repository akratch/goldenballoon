#!/usr/bin/env python3
"""ROM-backed smoke gate for the complete Taj/Wizpig/Terry roster.

This deliberately complements the deeper Taj-only picker regression. It arms
all three virtual identities at once, requires the full 13-slot layout, and
proves that each presentation pair can be composed from a real supported ROM.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_adventure_two import eeprom_image
from harness_utils import (DEFAULT_BUILD_DIR, config_block as save_config_block,
                           read_ppm, resolve_binary)


STATE_TEXT = """\
mod_roster_version=3
taj_unlocked=1
taj_migration_complete=1
wizpig_unlocked=1
wizpig_migration_complete=1
terry_unlocked=1
terry_migration_complete=1
"""
INPUT_TEXT = """\
# Title -> character select; let all thirteen actors settle, then traverse the
# expanded top row from Diddy through Drumstick to Wizpig and down to Terry.
# Confirm/deselect Wizpig and confirm Terry so all three pose states are gated.
1250 START 4
1330 START 4
1500 RIGHT 4
1550 RIGHT 4
1600 A 4
1620 B 4
1650 DOWN 4
1700 A 4
"""
ROSTER = (
    "mod_roster: base=10 count=13 taj=10 wizpig=11 terry=12 "
    "taj_enabled=1 wizpig_enabled=1 terry_enabled=1 drumstick=1 tt=1"
)


def complete_save() -> bytes:
    image = bytearray(eeprom_image(False))
    value = (1 << 25) | (1 << 1) | (0xFFFFF << 4)
    image[120:128] = save_config_block(value)
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--evidence-dir")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file():
        print("check_bonus_character_select: FAIL -- missing binary or ROM",
              file=sys.stderr)
        return 1

    temporary = args.evidence_dir is None
    if args.evidence_dir is None:
        root = Path(tempfile.mkdtemp(prefix="mdkr-bonus-select-"))
    else:
        root = Path(args.evidence_dir)
        root.mkdir(parents=True, exist_ok=True)
    save = root / "save"
    frames = root / "frames"
    save.mkdir(exist_ok=True)
    frames.mkdir(exist_ok=True)
    for stale_frame in frames.glob("frame_*.ppm"):
        stale_frame.unlink()
    (save / "eeprom.bin").write_bytes(complete_save())
    (save / "taj_mod_state.ini").write_text(STATE_TEXT, encoding="ascii")
    script = root / "select.txt"
    script.write_text(INPUT_TEXT, encoding="ascii")

    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_TAJ_SELECT_TRACE="1",
        MDKR_RENDERER="opengl",
        MDKR_RENDER_SCALE="1",
        MDKR_SAVE_DIR=str(save),
        MDKR_DUMP_FROM="1520",
        MDKR_DUMP_EVERY="10000",
        MDKR64_HIDDEN="1",
    )
    command = [
        str(binary), "--headless-frames", "1750", "--input-script",
        str(script), "--dump-frames", str(frames), "--rom", str(rom),
        "--window-size", "1280x960", "--restored",
    ]
    if args.verbose:
        print("$ " + " ".join(command), flush=True)
    process = subprocess.run(
        command, cwd=root, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=90, check=False)
    output = process.stdout or ""
    (root / "run.log").write_text(output, encoding="utf-8")

    failures: list[str] = []
    if process.returncode != 0:
        failures.append(f"exit code {process.returncode}")
    for marker in ("[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            failures.append(f"fatal marker {marker}")
    if ROSTER not in output:
        failures.append("missing contiguous 13-racer roster marker")
    if "[TAJSELECT] event=pair_compose" not in output:
        failures.append("Taj actor/sign pair did not compose")
    if "mod_select_visual: identity=WIZPIG actor=1 sign=1" not in output:
        failures.append("Wizpig actor/sign pair did not compose")
    if "mod_select_visual: identity=TERRY actor=1 sign=1" not in output:
        failures.append("Terry actor/sign pair did not compose")
    for identity in ("WIZPIG", "TERRY"):
        marker = (f"bonus_transform: identity={identity} header_racer=1 "
                  "racer_transform=0 scale_y=1.000")
        if marker not in output:
            failures.append(
                f"{identity} did not bypass the uninitialised racer transform")
    if "label=WIZPIG" not in output:
        failures.append("controller route did not reach Wizpig")
    if "label=TERRY" not in output:
        failures.append("controller route did not reach Terry")
    pose_markers = (
        "identity=WIZPIG animation=1 hover=0x0",
        "identity=WIZPIG animation=0 hover=0x1",
        "identity=WIZPIG animation=3 hover=0x1 confirmed=0x1",
        "identity=TERRY animation=2 hover=0x0",
        "identity=TERRY animation=0 hover=0x1",
        "identity=TERRY animation=4 hover=0x1 confirmed=0x1",
    )
    for marker in pose_markers:
        if f"mod_select_pose_state: {marker}" not in output:
            failures.append(f"missing menu pose transition: {marker}")
    dumps = sorted(frames.glob("frame_*.ppm"))
    if len(dumps) != 1:
        failures.append(f"expected one picker frame, got {len(dumps)}")
    else:
        width, height, pixels = read_ppm(dumps[0])
        if (width % 320 or height % 240 or width * 3 != height * 4 or
                len(set(pixels[::97])) < 32):
            failures.append("picker capture is missing or visually empty")

    if failures:
        print("check_bonus_character_select: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"  evidence: {root}", file=sys.stderr)
        return 1
    print("check_bonus_character_select: PASS -- 13 racers; Taj, Wizpig, and "
          "Terry presentation pairs composed")
    if args.evidence_dir:
        print(f"evidence: {root}")
    if temporary:
        shutil.rmtree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
