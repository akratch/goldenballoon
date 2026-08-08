#!/usr/bin/env python3
"""Build a genuinely-complete Adventure save using ONLY the mdkr-save CLI.

Why this exists: an earlier hand-packed 100% generator (a one-off script, not
checked in) hardcoded the number of eligible courses and hub worlds instead of
reading them from the ROM/engine, on the theory that a wrong field count would
silently shift every bit after it. That specific theory turned out to be
wrong -- the course/world counts it used (34 courses, 6 worlds) exactly match
platform/save_codec.h's MDKR_SAVE_COURSE_COUNT / MDKR_SAVE_WORLD_COUNT, which
are themselves derived from the same ROM level table at build time. The save
it produced decoded cleanly and passed `mdkr-save inspect`.

The actual bug was elsewhere: the generator set every defined cutscene bit,
including CUTSCENE_ADVENTURE_TWO (0x4). game/src/menu.c's file-select entry
gate (fileselect_input_root(), MENU_FILE_SELECT case 0) refuses to enter a
slot whose isAdventure2 marker (derived live from cutsceneFlags &
CUTSCENE_ADVENTURE_TWO) does not match whichever GAME_SELECT option the
player is currently under -- it just plays a buzzer and does nothing. The
generator also left the global "Adventure Two unlocked" config bit clear, so
GAME_SELECT never even offered the Adventure Two option that this save
actually needed. The save was not corrupt; it was permanently parked behind
the wrong menu.

This script avoids the whole bug *class*, not just this one instance: it does
not pack a single bit itself. Every field write goes through
platform/save_codec.c via the CLI, so course/world counts, field widths, and
checksums are always whatever the engine's own codec says they are. A 100%
save is genuinely a completed Adventure Two, so this save is built to match:
Adventure Two is globally unlocked (config field `adventure-two`) and the
slot's cutsceneFlags carry CUTSCENE_ADVENTURE_TWO, exactly like a real
completed run. tests/input_scripts/adventure_two_resume_race.txt already
encodes the one navigation this implies: GAME_SELECT's second option, not the
first, is what reaches this slot.

Usage:
    python3 tools/mdkr_save_make100.py CLI_BINARY OUTPUT.eep [--slot N] [--name XXX]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

# Real per-slot ceilings straight out of the field's own comments in
# game/include/structs.h and the thresholds save_data.c / game.c / object_functions.c
# actually branch on (grep for "settings->trophies", "settings->bosses",
# "balloonsPtr[" to re-derive these if the tables ever move):
#
#   trophies: 5 worlds (Dino, Sherbet, Snowflake, Dragon, FFL) x 2 bits, 0b11
#             (gold) each -> 0x3FF is ALL golds. The old generator used 0xFF,
#             which golds only 4 of the 5 worlds.
#   bosses:   10 defined single-bit flags (Wizpig/Tricky/Bubbler/Bluey/Smokey,
#             both rounds); bit 0x40 is not assigned to anything -> 0x7BF is
#             every defined boss beaten twice, without setting the unknown bit.
#   cutscenes: every defined CUTSCENE_* bit from structs.h; 0x800 and 0x1000
#             are gaps (no #define), so left at 0 rather than guessed.
#   balloons: index 0 is the running TOTAL across the five real worlds
#             (object_functions.c explicitly excludes WORLD_CENTRAL_AREA from
#             it), capped at 47 by the one literal check in game.c
#             ("balloonsPtr[0] >= 47"); it is also divided by 10 for an
#             on-screen two-digit sprite, so anything above 99 would read
#             garbage digits -- 47 is both the real ceiling and the only safe
#             display value. Indices 1..5 (the five hub worlds) cap the
#             per-world unlock checks (">= 8" for the boss rematch door) at 8.
TROPHIES_ALL_GOLD = 0x3FF
BOSSES_ALL_BEATEN = 0x7BF
CUTSCENES_ALL_SEEN = 0x3E7FF
BALLOON_TOTAL = 47
BALLOON_PER_WORLD = 8
WORLD_FLAGS_ALL_OPEN = 0xFFFF
TT_COURSES_ALL_BEATEN = 0xFFFFF  # 20 T.T. staff ghosts; also the TT-unlock test.


def run_cli(cli: str, *args: str) -> None:
    result = subprocess.run([cli, *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"mdkr-save {' '.join(args)} failed (exit {result.returncode}):\n"
            f"{result.stdout}{result.stderr}"
        )


def inspect(cli: str, path: str) -> dict:
    result = subprocess.run([cli, "inspect", path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"mdkr-save inspect failed: {result.stdout}{result.stderr}")
    return json.loads(result.stdout)


def build(cli: str, output_path: str, slot: int, name: str,
          adventure_one: bool = False) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr_make100_") as tmp:
        work = str(Path(tmp) / "work.eep")
        Path(work).write_bytes(b"\xFF" * 512)

        # Discover the course/world counts from the tool itself -- never a
        # literal in this script -- so a ROM/engine change can never silently
        # desync the loop bounds below from what the codec actually encodes.
        blank = inspect(cli, work)
        course_count = len(blank["slots"][slot]["courses"])
        world_count = len(blank["slots"][slot]["balloons"])
        assert world_count == len(blank["slots"][slot]["worldFlags"])

        run_cli(cli, "edit-slot-state", work, work, str(slot), "create")
        run_cli(cli, "edit-slot-name", work, work, str(slot), name)

        run_cli(cli, "edit-course-all", work, work, str(slot), "3")  # visited+cleared+silver

        run_cli(cli, "edit-balloon-all", work, work, str(slot), str(BALLOON_PER_WORLD))
        run_cli(cli, "edit-balloon", work, work, str(slot), "0", str(BALLOON_TOTAL))

        run_cli(cli, "edit-world-flags-all", work, work, str(slot), str(WORLD_FLAGS_ALL_OPEN))

        run_cli(cli, "edit-slot-field", work, work, str(slot), "taj-flags", "0x3F")
        run_cli(cli, "edit-slot-field", work, work, str(slot), "trophies", str(TROPHIES_ALL_GOLD))
        run_cli(cli, "edit-slot-field", work, work, str(slot), "bosses", str(BOSSES_ALL_BEATEN))
        run_cli(cli, "edit-slot-field", work, work, str(slot), "tt-amulet", "4")
        run_cli(cli, "edit-slot-field", work, work, str(slot), "wizpig-amulet", "4")
        run_cli(cli, "edit-slot-field", work, work, str(slot), "keys", "0xFF")
        # --adventure-one clears CUTSCENE_ADVENTURE_TWO so the slot is an
        # ADVENTURE ONE file. That is a real, reachable state -- everything
        # cleared, Adventure Two unlocked but not yet started -- and it is the
        # one worth handing a tester, because Adventure Two remaps the courses
        # and a regression sweep run there is not comparing like with like.
        # The global unlock stays on either way: it is what 100% completion
        # earns, and leaving it off is precisely what stranded the earlier
        # save behind a GAME_SELECT option that was never offered.
        cutscenes = (CUTSCENES_ALL_SEEN & ~0x4) if adventure_one \
            else CUTSCENES_ALL_SEEN
        run_cli(cli, "edit-slot-field", work, work, str(slot), "cutscenes", str(cutscenes))

        run_cli(cli, "edit-config", work, work, "adventure-two", "1")
        run_cli(cli, "edit-config", work, work, "drumstick", "1")
        run_cli(cli, "edit-config", work, work, "language", "0")
        run_cli(cli, "edit-config", work, work, "tt-courses", str(TT_COURSES_ALL_BEATEN))
        run_cli(cli, "edit-config", work, work, "default", "1")
        run_cli(cli, "edit-config", work, work, "subtitles", "1")

        run_cli(cli, "reset-records", work, work, "3")

        final = inspect(cli, work)
        slot_summary = final["slots"][slot]
        assert slot_summary["status"] == "valid", slot_summary
        assert len(slot_summary["courses"]) == course_count
        assert all(status == 3 for status in slot_summary["courses"])
        assert slot_summary["ttAmulet"] == 4
        assert slot_summary["wizpigAmulet"] == 4
        if adventure_one:
            assert not (slot_summary["cutscenes"] & 0x4), (
                "an Adventure One file must NOT carry CUTSCENE_ADVENTURE_TWO, "
                "or FILE SELECT refuses it under the Adventure One option"
            )
        else:
            assert slot_summary["cutscenes"] & 0x4, (
                "CUTSCENE_ADVENTURE_TWO must be set for a completed-Adventure-Two save"
            )
        assert final["config"]["adventureTwo"] == 1, (
            "config.adventure-two must be unlocked to match a save whose "
            "cutsceneFlags claim Adventure Two is complete"
        )

        Path(output_path).write_bytes(Path(work).read_bytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cli", help="path to the mdkr-save CLI binary")
    ap.add_argument("output", help="destination raw .eep (512 bytes)")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--adventure-one", action="store_true",
                    help="produce an Adventure ONE file, entered from the first "
                         "GAME SELECT option; default is a completed-Adventure-Two file")
    ap.add_argument("--name", default="ACE")
    args = ap.parse_args()
    build(args.cli, args.output, args.slot, args.name, args.adventure_one)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
