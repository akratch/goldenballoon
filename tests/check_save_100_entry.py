#!/usr/bin/env python3
"""Regression check for the "100% save shows up but can't be entered" bug.

History: a hand-packed 100% Adventure save decoded cleanly, passed
`mdkr-save inspect`, and rendered in FILE SELECT -- but pressing A on it did
nothing. The cause was not a bit-alignment bug in the packer (its course/world
counts matched platform/save_codec.h's MDKR_SAVE_COURSE_COUNT /
MDKR_SAVE_WORLD_COUNT exactly). It was that the packer set every defined
cutscene bit, including CUTSCENE_ADVENTURE_TWO, while leaving the global
"Adventure Two unlocked" config bit clear. game/src/menu.c's FILE_SELECT entry
gate (fileselect_input_root(), case 0) silently refuses to enter a slot whose
isAdventure2 marker doesn't match whichever GAME_SELECT option is currently
selected -- it just plays a buzzer and returns. `mdkr-save inspect` and a
headless boot cannot see this: both only decode the slot, they never drive the
FILE_SELECT input gate.

This check drives that gate for real, with two arms:

* good: tools/mdkr_save_make100.py's output, entered through GAME_SELECT's
  Adventure Two option (tests/input_scripts/adventure_two_resume_race.txt) --
  the route that slot's own cutsceneFlags commit it to. Must actually enter
  (a level_load past FILE_SELECT), not just render.
* broken (positive control): a save with the same AT2-complete cutsceneFlags
  but the global Adventure Two config bit left unset -- the exact defect
  class that shipped. GAME_SELECT then never offers the Adventure Two option
  the slot needs, so driving the Adventure One route (the only one on offer)
  must reach FILE_SELECT and then get silently refused, proving this check
  would have caught the historical bug rather than rubber-stamping it.

Usage:
    python3 tests/check_save_100_entry.py --build build-rel
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from harness_utils import ASSERT_MARKERS, DEFAULT_BUILD_DIR, fatal_re, resolve_binary  # noqa: E402
import mdkr_save_make100  # noqa: E402

FILE_SELECT_MENU_ID = 6
MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LEVEL_LOAD_RE = re.compile(r"level_load: levelId=(-?\d+).*@frame~(\d+)")
SANITIZER_RE = re.compile(
    r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:"
)
FATAL_RE = fatal_re(*ASSERT_MARKERS)

ADV_TWO_SCRIPT = Path("tests/input_scripts/adventure_two_resume_race.txt")
ADV_ONE_SCRIPT = Path("tests/input_scripts/adventure_resume_race.txt")


def resolve_cli(build: str) -> Path:
    """The mdkr-save CLI lives beside the mdkr64 binary in the same build dir."""

    binary = Path(resolve_binary(build)).resolve()
    cli = binary.parent / "mdkr-save"
    return cli


def make_mismatched_save(cli: Path, output: Path) -> None:
    """The historical defect: AT2-complete cutsceneFlags, AT2 NOT unlocked globally."""

    work = str(output)
    Path(work).write_bytes(b"\xFF" * 512)
    mdkr_save_make100.run_cli(str(cli), "edit-slot-state", work, work, "0", "create")
    mdkr_save_make100.run_cli(str(cli), "edit-slot-name", work, work, "0", "ACE")
    mdkr_save_make100.run_cli(str(cli), "edit-course-all", work, work, "0", "3")
    mdkr_save_make100.run_cli(str(cli), "edit-balloon-all", work, work, "0",
                              str(mdkr_save_make100.BALLOON_PER_WORLD))
    mdkr_save_make100.run_cli(str(cli), "edit-balloon", work, work, "0", "0",
                              str(mdkr_save_make100.BALLOON_TOTAL))
    mdkr_save_make100.run_cli(str(cli), "edit-world-flags-all", work, work, "0",
                              str(mdkr_save_make100.WORLD_FLAGS_ALL_OPEN))
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "taj-flags", "0x3F")
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "trophies", str(mdkr_save_make100.TROPHIES_ALL_GOLD))
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "bosses", str(mdkr_save_make100.BOSSES_ALL_BEATEN))
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "tt-amulet", "4")
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "wizpig-amulet", "4")
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "keys", "0xFF")
    mdkr_save_make100.run_cli(str(cli), "edit-slot-field", work, work, "0",
                              "cutscenes", str(mdkr_save_make100.CUTSCENES_ALL_SEEN))
    # The defect: leave config's adventure-two bit UNSET, unlike make100.py.
    mdkr_save_make100.run_cli(str(cli), "edit-config", work, work, "drumstick", "1")
    mdkr_save_make100.run_cli(str(cli), "edit-config", work, work, "default", "1")
    mdkr_save_make100.run_cli(str(cli), "edit-config", work, work, "subtitles", "1")
    mdkr_save_make100.run_cli(str(cli), "reset-records", work, work, "3")


def drive(binary: Path, rom: Path, save_dir: Path, script: Path, frames: int,
         timeout: int) -> tuple[str, int]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR_")}
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_TRACE": "1",
        "MDKR_SAVE_DIR": str(save_dir),
    })
    cmd = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", str(rom),
    ]
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout, check=False)
    return proc.stdout.decode("utf-8", "replace"), proc.returncode


def entered_gameplay_after_file_select(output: str) -> bool:
    """True iff a level_load appears at/after the frame FILE_SELECT was reached.

    The FILE_SELECT entry gate either transitions to a new menu/level (success)
    or does nothing at all (refused): there is no third outcome to distinguish,
    so "a level_load happened after menu 6 rendered" is exactly "A was
    accepted".
    """

    menus = [(int(m.group(1)), int(m.group(2))) for m in MENU_RE.finditer(output)]
    file_select_frames = [frame for menu_id, frame in menus if menu_id == FILE_SELECT_MENU_ID]
    if not file_select_frames:
        return False
    reached_at = file_select_frames[0]
    loads = [int(m.group(2)) for m in LEVEL_LOAD_RE.finditer(output)]
    return any(frame > reached_at for frame in loads)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    cli = resolve_cli(args.build)
    rom = Path(args.rom).expanduser().resolve()
    adv2_script = ADV_TWO_SCRIPT.resolve()
    adv1_script = ADV_ONE_SCRIPT.resolve()
    for label, path in (("mdkr64 binary", binary), ("mdkr-save CLI", cli),
                        ("ROM", rom), ("Adv2 script", adv2_script),
                        ("Adv1 script", adv1_script)):
        if not path.is_file():
            print(f"check_save_100_entry: FAIL — missing {label}: {path}",
                  file=sys.stderr)
            return 1

    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr_save100_") as tmp:
        root = Path(tmp)
        good_save = root / "good.eep"
        broken_save = root / "broken.eep"
        mdkr_save_make100.build(str(cli), str(good_save), slot=0, name="ACE")
        make_mismatched_save(cli, broken_save)

        good_dir = root / "good_save"
        good_dir.mkdir()
        (good_dir / "eeprom.bin").write_bytes(good_save.read_bytes())
        good_output, good_rc = drive(binary, rom, good_dir, adv2_script,
                                     args.frames, args.timeout)
        good_entered = entered_gameplay_after_file_select(good_output)
        if good_rc != 0:
            failures.append(f"good arm: runner exited {good_rc}")
        if SANITIZER_RE.search(good_output) or FATAL_RE.search(good_output):
            failures.append("good arm: sanitizer/fatal marker in output")
        if not good_entered:
            failures.append(
                "good arm: mdkr_save_make100.py's save reached FILE_SELECT but "
                "was never entered via the Adventure Two route -- this is "
                "exactly the historical bug"
            )

        broken_dir = root / "broken_save"
        broken_dir.mkdir()
        (broken_dir / "eeprom.bin").write_bytes(broken_save.read_bytes())
        broken_output, broken_rc = drive(binary, rom, broken_dir, adv1_script,
                                         args.frames, args.timeout)
        broken_entered = entered_gameplay_after_file_select(broken_output)
        if broken_rc != 0:
            failures.append(f"broken control: runner exited {broken_rc}")
        if SANITIZER_RE.search(broken_output) or FATAL_RE.search(broken_output):
            failures.append("broken control: sanitizer/fatal marker in output")
        if broken_entered:
            failures.append(
                "broken control: a save with CUTSCENE_ADVENTURE_TWO set but "
                "config.adventure-two unset was entered via the Adventure One "
                "route -- this check would not have caught the historical bug"
            )

    if failures:
        print("check_save_100_entry: FAIL", file=sys.stderr)
        for failure in failures:
            print("  - " + failure, file=sys.stderr)
        if args.verbose:
            print("--- good arm output tail ---", file=sys.stderr)
            print("\n".join(good_output.splitlines()[-60:]), file=sys.stderr)
            print("--- broken control output tail ---", file=sys.stderr)
            print("\n".join(broken_output.splitlines()[-60:]), file=sys.stderr)
        return 1

    print("check_save_100_entry: PASS — a genuine 100% save enters via "
          "Adventure Two, and a save carrying the historical config/cutscene "
          "mismatch is correctly refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
