#!/usr/bin/env python3
"""The game itself still reads a 1.7.0 save as the same progress.

tests/test_save_v170_fixture.c and tests/test_save_v170_inspect.py hold the
ROM-free save tooling to tests/fixtures/save_v170. Neither can see the codec a
player's save actually goes through at boot: game/src/save_data.c's
populate_settings_from_save_data() and func_800732E8(). That codec walks the
ROM's level table to decide which courses a slot stores, so its layout depends
on the level count it is handed -- and 1.8 changed where that count comes from
(level_count_for_save()). A different count moves every field after the course
block, and the game would read a 1.7.0 Adventure as someone else's.

MDKR_SAVE_COMPAT_FIXTURE (game/src/save_inventory_selftest.c) hands the game's
own codec the 1.7.0 image after level-table init, prints what it decoded and
re-encodes it. This check asserts:

1. **The same facts 1.7.0 reported.** Every field 1.7.0's `mdkr-save inspect`
   printed for this image (inspect-1.7.0.json) -- each course's state in save
   order, balloons per world, world flags, amulets, keys, cutscene flags, the
   file name -- is what the game decodes now.
2. **The course the 1.7.0 run won is the one marked cleared.** Save index 3
   must be level 5, Ancient Lake: the race the 1.7.0 route drove and won.
3. **Byte-identical re-encode** of the slot, and the blocks 1.7.0 left empty
   read as empty.
4. **A mis-laid-out save is caught.** The same image with the slot's fields
   shifted one bit (checksum resealed, so the game still accepts it) must fail
   assertion 1. Without that arm, a comparison that read nothing would pass.
5. **The settings word the route never wrote still round-trips.** 1.7.0 left
   it empty, so the image is re-run with a checksum-valid settings word
   (subtitles on, Adventure Two unlocked) in its place: the game must accept
   it and re-encode it byte for byte, and must refuse one whose checksum is off
   by one.

Usage: tests/check_save_v170_compat.py --build build-rel --rom baserom.us.v80.z64
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import (DEFAULT_BUILD_DIR, SLOT_BYTES, config_block, find_fatal,
                           read_hex_fixture, resolve_binary, seal_slot)

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "fixtures" / "save_v170"
FRAMES = 30
ANCIENT_LAKE = 5
CLEARED_INDEX = 3
FILENAME_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.? "

SLOT_RE = re.compile(
    r"\[SAVE_COMPAT\] slot=(\d) status=valid taj=(\d+) trophies=(\d+) bosses=(\d+) "
    r"ttAmulet=(\d+) wizpigAmulet=(\d+) keys=(\d+) cutscenes=(\d+) filename=(\d+)")
LIST_RE = re.compile(r"\[SAVE_COMPAT\] slot=(\d) (courses|balloons|worldFlags)=(\S*)")
EMPTY_RE = re.compile(r"\[SAVE_COMPAT\] (slot=\d|records|config) status=empty")
REENCODE_RE = re.compile(r"\[SAVE_COMPAT\] (\S+) reencode=(identical|DIFFERENT) bytes=(\d+)")
CONFIG_RE = re.compile(r"\[SAVE_COMPAT\] config value=0x([0-9a-f]{16}) checksum=(valid|INVALID)")
CONFIG_OFFSET = 120
# Subtitles (bit 25) and Adventure Two (bit 0), as the options menu writes them.
CONFIG_PAYLOAD = (1 << 25) | 1
DONE_RE = re.compile(
    r"\[SAVE_COMPAT\] done slots=(\d+) records=(\d+) config=(\d+) mismatches=(\d+)")


def encode_filename(name: str) -> int:
    """mdkr_save_filename_encode(): three 5-bit letters, a short name padded."""

    value = 0
    padded = (name + "   ")[:3]
    for letter in padded:
        value = value * 32 + FILENAME_ALPHABET.index(letter)
    return value


def shifted_slot(image: bytes) -> bytes:
    """The image with slot 1's fields moved one bit later, checksum resealed.

    This is the save a codec with one extra course bit would write. The game
    accepts it (the checksum is valid), so only the decoded facts can reject it.
    """

    bits = "".join(f"{byte:08b}" for byte in image[2:SLOT_BYTES])
    bits = ("0" + bits)[:len(bits)]
    slot = bytearray(image[:2]) + bytearray(
        int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
    return bytes(seal_slot(slot)) + image[SLOT_BYTES:]


def with_config(image: bytes, word: bytes) -> bytes:
    return image[:CONFIG_OFFSET] + word + image[CONFIG_OFFSET + 8:]


def run_seam(binary: Path, rom: Path, image: bytes, label: str, root: Path,
             verbose: bool) -> tuple[int, str]:
    run_dir = root / label
    (run_dir / "save").mkdir(parents=True)
    fixture = run_dir / "eeprom-under-test.bin"
    fixture.write_bytes(image)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        MDKR_AUDIO="0",
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR_SAVE_COMPAT_FIXTURE=str(fixture),
    )
    command = [str(binary), "--headless-frames", str(FRAMES), "--rom", str(rom)]
    if verbose:
        print(f"$ ({label}) MDKR_SAVE_COMPAT_FIXTURE={fixture} {' '.join(command)}")
    process = subprocess.run(command, cwd=run_dir, env=env, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=180, check=False)
    return process.returncode, process.stdout or ""


def fact_differences(expected: dict, output: str) -> list[str]:
    """Where the game's decode of slot 1 disagrees with 1.7.0's report."""

    slot = expected["slots"][0]
    found: list[str] = []
    match = next((m for m in SLOT_RE.finditer(output) if m.group(1) == "0"), None)
    if match is None:
        return ["the game did not decode slot 1 as a valid Adventure file"]
    names = ("tajFlags", "trophies", "bosses", "ttAmulet", "wizpigAmulet", "keys",
             "cutscenes")
    for index, name in enumerate(names, 2):
        if int(match.group(index)) != slot[name]:
            found.append(f"{name}: 1.7.0 said {slot[name]}, the game decodes "
                         f"{match.group(index)}")
    if int(match.group(9)) != encode_filename(slot["name"]):
        found.append(f"file name: 1.7.0 said {slot['name']!r} "
                     f"({encode_filename(slot['name'])}), the game decodes {match.group(9)}")
    lists = {m.group(2): m.group(3) for m in LIST_RE.finditer(output) if m.group(1) == "0"}
    courses = [entry.split(":") for entry in lists.get("courses", "").split(",") if entry]
    statuses = [int(status) for _level, status in courses]
    if statuses != slot["courses"]:
        found.append(f"course states in save order: 1.7.0 said {slot['courses']}, the game "
                     f"decodes {statuses}")
    for name in ("balloons", "worldFlags"):
        values = [int(v) for v in lists.get(name, "").split(",") if v]
        if values != slot[name]:
            found.append(f"{name}: 1.7.0 said {slot[name]}, the game decodes {values}")
    if len(courses) > CLEARED_INDEX and courses[CLEARED_INDEX] != [str(ANCIENT_LAKE), "2"]:
        found.append(f"save index {CLEARED_INDEX} is level {courses[CLEARED_INDEX][0]} with state "
                     f"{courses[CLEARED_INDEX][1]}; the 1.7.0 run cleared Ancient Lake "
                     f"(level {ANCIENT_LAKE})")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.exists():
            print(f"check_save_v170_compat: FAIL\n  - missing {path}")
            return 1
    image = read_hex_fixture(FIXTURE / "eeprom.hex")
    expected = json.loads((FIXTURE / "inspect-1.7.0.json").read_text(encoding="utf-8"))

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-save-v170-") as tmp:
        root = Path(tmp)
        code, output = run_seam(binary, rom, image, "fixture", root, args.verbose)
        control_code, control = run_seam(binary, rom, shifted_slot(image), "shifted",
                                         root, args.verbose)
        word = config_block(CONFIG_PAYLOAD)
        config_code, config = run_seam(binary, rom, with_config(image, word), "config",
                                       root, args.verbose)
        bad_word = bytes([(word[0] + 1) & 0xFF]) + word[1:]
        bad_code, bad = run_seam(binary, rom, with_config(image, bad_word), "config-bad",
                                 root, args.verbose)

    for label, rc, text in (("1.7.0 image", code, output),
                            ("shifted control", control_code, control),
                            ("settings arm", config_code, config),
                            ("bad settings control", bad_code, bad)):
        if rc != 0:
            failures.append(f"{label}: exit {rc}")
        marker = find_fatal(text)
        if marker:
            failures.append(f"{label}: log contains {marker!r}")
        if DONE_RE.search(text) is None:
            failures.append(f"{label}: the MDKR_SAVE_COMPAT_FIXTURE seam never reported; "
                            "the build predates it or the image was not read")

    failures += fact_differences(expected, output)
    done = DONE_RE.search(output)
    if done is not None and done.groups() != ("1", "0", "0", "0"):
        failures.append(f"seam summary: slots/records/config/mismatches = "
                        f"{'/'.join(done.groups())}, expected 1/0/0/0")
    reencoded = {m.group(1): (m.group(2), m.group(3)) for m in REENCODE_RE.finditer(output)}
    if reencoded.get("slot=0") != ("identical", str(SLOT_BYTES)):
        failures.append(f"slot 1 did not re-encode byte for byte: {reencoded.get('slot=0')}")
    empties = set(EMPTY_RE.findall(output))
    for block in ("slot=1", "slot=2", "records", "config"):
        if block not in empties:
            failures.append(f"{block}: 1.7.0 left it empty, the game did not read it as empty")

    decoded = CONFIG_RE.search(config)
    if decoded is None or decoded.groups() != (word.hex(), "valid"):
        failures.append(f"settings arm: expected value=0x{word.hex()} checksum=valid, got "
                        f"{decoded.groups() if decoded else 'no settings line'}")
    if ("config", "identical", "8") not in REENCODE_RE.findall(config):
        failures.append("settings arm: the settings word did not re-encode byte for byte")
    done = DONE_RE.search(config)
    if done is not None and done.groups() != ("1", "0", "1", "0"):
        failures.append(f"settings arm: summary {'/'.join(done.groups())}, expected 1/0/1/0")
    rejected = CONFIG_RE.search(bad)
    if rejected is None or rejected.group(2) != "INVALID":
        failures.append("bad settings control: a settings word with a wrong checksum was "
                        "not reported INVALID")

    control_found = fact_differences(expected, control)
    if not control_found:
        failures.append("positive control failed: a slot shifted by one bit decoded to the "
                        "same facts, so the comparison above cannot see a layout change")

    if failures:
        print("check_save_v170_compat: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        if args.verbose:
            print("\n".join(line for line in output.splitlines() if "SAVE_COMPAT" in line))
        return 1
    print("check_save_v170_compat: PASS -- the game decodes the 1.7.0 save to every fact "
          "1.7.0 reported (Ancient Lake cleared at save index 3) and re-encodes the slot "
          f"byte for byte; the one-bit-shifted control is rejected ({len(control_found)} "
          "differing facts); a settings word round-trips and a bad one is refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
