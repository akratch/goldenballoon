#!/usr/bin/env python3
"""Future Fun Land trophy award/display/save through Adventure Two menus.

Uses the existing trophy finish/order seams, not four manually driven races.
Fresh-process FFL cabinet reload is separate coverage.
"""
import argparse
from pathlib import Path

from check_adventure_two import slot_cutscene_flags, CUTSCENE_ADVENTURE_TWO
from check_trophy_series import (
    AWARD_RE, BAD_RE, ROUND_RE, GOLD_ORDER, EEPROM_BYTES, SLOT_BYTES,
    TROPHY_BIT_OFFSET, eeprom_image, read_bits, run_case, slot_checksum,
)
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", required=True, type=Path)
    args = parser.parse_args()
    process, save = run_case(Path(resolve_binary(args.build)).resolve(),
                             args.rom.resolve(), 13000, eeprom_image(True),
                             "full", GOLD_ORDER, 5, adventure_two=True)
    output = process.stdout
    failures = []
    if process.returncode or BAD_RE.search(output):
        failures.append("native process failed")
    tracks = [int(m.group(3)) for m in ROUND_RE.finditer(output)][:4]
    if tracks != [17, 32, 33, 15]:
        failures.append(f"incorrect championship tracks: {tracks}")
    for track in (17, 32, 33, 15):
        if f"adventure_mode: level={track} adventureTwo=1 mirrored=1" not in output:
            failures.append(f"missing Adventure Two witness on track {track}")
    award = AWARD_RE.search(output)
    if award is None or (int(award[1]), int(award[2]), int(award[3]), int(award[5], 16)) != (5, 0, 36, 0x300):
        failures.append("missing FFL gold award")
    if "trophycabinet: DISPLAY world=5 state=3 trophies=0x300" not in output:
        failures.append("FFL cabinet did not display gold")
    if len(save) != EEPROM_BYTES:
        failures.append("invalid EEPROM length")
    else:
        slot = save[:SLOT_BYTES]
        if int.from_bytes(slot[:2], "big") != slot_checksum(slot):
            failures.append("invalid saved checksum")
        if read_bits(slot, TROPHY_BIT_OFFSET, 10) != 0x300:
            failures.append("FFL gold not persisted")
        if not slot_cutscene_flags(slot) & CUTSCENE_ADVENTURE_TWO:
            failures.append("Adventure Two save identity lost")
    if failures:
        print(output[-8000:])
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("check_trophy_adventure_two: PASS — FFL gold award, mirrored races, cabinet and EEPROM")


if __name__ == "__main__":
    main()
